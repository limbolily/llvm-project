//===- lib/ReaderWriter/ELF/File.h ----------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_READER_WRITER_ELF_FILE_H
#define LLD_READER_WRITER_ELF_FILE_H

#include "Atoms.h"

#include "lld/Core/File.h"
#include "lld/Core/Reference.h"
#include "lld/ReaderWriter/ELFTargetInfo.h"
#include "lld/ReaderWriter/ReaderArchive.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Memory.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/system_error.h"

#include <map>
#include <unordered_map>

namespace lld {
namespace elf {
/// \brief Read a binary, find out based on the symbol table contents what kind
/// of symbol it is and create corresponding atoms for it
template <class ELFT> class ELFFile : public File {
  typedef llvm::object::Elf_Sym_Impl<ELFT> Elf_Sym;
  typedef llvm::object::Elf_Shdr_Impl<ELFT> Elf_Shdr;
  typedef llvm::object::Elf_Rel_Impl<ELFT, false> Elf_Rel;
  typedef llvm::object::Elf_Rel_Impl<ELFT, true> Elf_Rela;

  // A Map is used to hold the atoms that have been divided up
  // after reading the section that contains Merge String attributes
  struct MergeSectionKey {
    MergeSectionKey(const Elf_Shdr *shdr, int32_t offset)
        : _shdr(shdr), _offset(offset) {
    }
    // Data members
    const Elf_Shdr *_shdr;
    int32_t _offset;
  };
  struct MergeSectionEq {
    int64_t operator()(const MergeSectionKey &k) const {
      return llvm::hash_combine((int64_t)(k._shdr->sh_name),
                                (int64_t) k._offset);
    }
    bool operator()(const MergeSectionKey &lhs,
                    const MergeSectionKey &rhs) const {
      return ((lhs._shdr->sh_name == rhs._shdr->sh_name) &&
              (lhs._offset == rhs._offset));
    }
  };

  struct MergeString {
    MergeString(int32_t offset, StringRef str, const Elf_Shdr *shdr,
                StringRef sectionName)
        : _offset(offset), _string(str), _shdr(shdr),
          _sectionName(sectionName) {
    }
    // the offset of this atom
    int32_t _offset;
    // The content
    StringRef _string;
    // Section header
    const Elf_Shdr *_shdr;
    // Section name
    StringRef _sectionName;
  };

  // This is used to find the MergeAtom given a relocation
  // offset
  typedef std::vector<ELFMergeAtom<ELFT> *> MergeAtomsT;
  typedef typename MergeAtomsT::iterator MergeAtomsIter;

  /// \brief find a mergeAtom given a start offset
  struct FindByOffset {
    const Elf_Shdr *_shdr;
    uint64_t _offset;
    FindByOffset(const Elf_Shdr *shdr, uint64_t offset)
        : _shdr(shdr), _offset(offset) {
    }
    bool operator()(const ELFMergeAtom<ELFT> *a) {
      uint64_t off = a->offset();
      return (_shdr->sh_name == a->section()) &&
             ((_offset >= off) && (_offset <= off + a->size()));
    }
  };

  /// \brief find a merge atom given a offset
  MergeAtomsIter findMergeAtom(const Elf_Shdr *shdr, uint64_t offset) {
    return std::find_if(_mergeAtoms.begin(), _mergeAtoms.end(),
                        FindByOffset(shdr, offset));
  }

  typedef std::unordered_map<MergeSectionKey, DefinedAtom *, MergeSectionEq,
                             MergeSectionEq> MergedSectionMapT;
  typedef typename MergedSectionMapT::iterator MergedSectionMapIterT;

public:
  ELFFile(const ELFTargetInfo &ti, StringRef name)
      : File(name, kindObject), _elfTargetInfo(ti) {}

  ELFFile(const ELFTargetInfo &ti, std::unique_ptr<llvm::MemoryBuffer> MB,
          llvm::error_code &EC)
      : File(MB->getBufferIdentifier(), kindObject), _elfTargetInfo(ti),
        _ordinal(0), _doStringsMerge(false) {
    llvm::OwningPtr<llvm::object::Binary> binaryFile;
    EC = createBinary(MB.release(), binaryFile);
    if (EC)
      return;

    // Point Obj to correct class and bitwidth ELF object
    _objFile.reset(
        llvm::dyn_cast<llvm::object::ELFObjectFile<ELFT> >(binaryFile.get()));

    if (!_objFile) {
      EC = make_error_code(llvm::object::object_error::invalid_file_type);
      return;
    }

    binaryFile.take();

    _doStringsMerge = _elfTargetInfo.mergeCommonStrings();

    // Read input sections from the input file
    // that need to be converted to
    // atoms
    if (createAtomizableSections(EC))
      return;

    // For mergeable strings, we would need to split the section
    // into various atoms
    if (createMergeableAtoms(EC))
      return;

    // Create the necessary symbols that are part of the section
    // that we created in createAtomizableSections function
    if (createSymbolsFromAtomizableSections(EC))
      return;

    // Create the appropriate atoms fom the file
    if (createAtoms(EC))
      return;
  }

  /// \brief Read input sections and populate necessary data structures
  /// to read them later and create atoms
  bool createAtomizableSections(llvm::error_code &EC) {
    // Handle: SHT_REL and SHT_RELA sections:
    // Increment over the sections, when REL/RELA section types are found add
    // the contents to the RelocationReferences map.
    llvm::object::section_iterator sit(_objFile->begin_sections());
    llvm::object::section_iterator sie(_objFile->end_sections());
    // Record the number of relocs to guess at preallocating the buffer.
    uint64_t totalRelocs = 0;
    for (; sit != sie; sit.increment(EC)) {
      if (EC)
        return true;

      const Elf_Shdr *section = _objFile->getElfSection(sit);

      if (isIgnoredSection(section))
        continue;

      if (isMergeableSection(section)) {
        _mergeStringSections.push_back(section);
        continue;
      }

      // Create a sectionSymbols entry for every progbits section.
      if (section->sh_type == llvm::ELF::SHT_PROGBITS)
        _sectionSymbols[section];

      if (section->sh_type == llvm::ELF::SHT_RELA) {
        StringRef sectionName;
        if ((EC = _objFile->getSectionName(section, sectionName)))
          return true;
        // Get rid of the leading .rela so Atoms can use their own section
        // name to find the relocs.
        sectionName = sectionName.drop_front(5);

        auto rai(_objFile->beginELFRela(section));
        auto rae(_objFile->endELFRela(section));

        _relocationAddendReferences[sectionName] = make_range(rai, rae);
        totalRelocs += std::distance(rai, rae);
      }

      if (section->sh_type == llvm::ELF::SHT_REL) {
        StringRef sectionName;
        if ((EC = _objFile->getSectionName(section, sectionName)))
          return true;
        // Get rid of the leading .rel so Atoms can use their own section
        // name to find the relocs.
        sectionName = sectionName.drop_front(4);

        auto ri(_objFile->beginELFRel(section));
        auto re(_objFile->endELFRel(section));

        _relocationReferences[sectionName] = make_range(ri, re);
        totalRelocs += std::distance(ri, re);
      }
    }
    _references.reserve(totalRelocs);
    return false;
  }

  /// \brief Create mergeable atoms from sections that have the merge attribute
  /// set
  bool createMergeableAtoms(llvm::error_code &EC) {

    // Divide the section that contains mergeable strings into tokens
    // TODO
    // a) add resolver support to recognize multibyte chars
    // b) Create a seperate section chunk to write mergeable atoms
    std::vector<MergeString *> tokens;
    for (auto msi : _mergeStringSections) {
      StringRef sectionContents;
      StringRef sectionName;
      if ((EC = _objFile->getSectionName(msi, sectionName)))
        return true;

      if ((EC = _objFile->getSectionContents(msi, sectionContents)))
        return true;

      unsigned int prev = 0;
      for (std::size_t i = 0, e = sectionContents.size(); i != e; ++i) {
        if (sectionContents[i] == '\0') {
          tokens.push_back(new (_readerStorage) MergeString(
              prev, sectionContents.slice(prev, i + 1), msi, sectionName));
          prev = i + 1;
        }
      }
    }

    // Create Mergeable atoms
    for (auto tai : tokens) {
      ArrayRef<uint8_t> content((const uint8_t *)tai->_string.data(),
                                tai->_string.size());
      ELFMergeAtom<ELFT> *mergeAtom = new (_readerStorage) ELFMergeAtom<ELFT>(
          *this, tai->_sectionName, tai->_shdr, content, tai->_offset);
      const MergeSectionKey mergedSectionKey(tai->_shdr, tai->_offset);
      if (_mergedSectionMap.find(mergedSectionKey) == _mergedSectionMap.end())
        _mergedSectionMap.insert(std::make_pair(mergedSectionKey, mergeAtom));
      mergeAtom->setOrdinal(++_ordinal);
      _definedAtoms._atoms.push_back(mergeAtom);
      _mergeAtoms.push_back(mergeAtom);
    }
    return false;
  }

  /// \brief Add the symbols that the sections contain. The symbols will be
  /// converted to atoms for
  /// Undefined symbols, absolute symbols
  bool createSymbolsFromAtomizableSections(llvm::error_code &EC) {
    llvm::object::section_iterator sit(_objFile->begin_sections());

    // Increment over all the symbols collecting atoms and symbol names for
    // later use.
    llvm::object::symbol_iterator it(_objFile->begin_symbols());
    llvm::object::symbol_iterator ie(_objFile->end_symbols());

    for (; it != ie; it.increment(EC)) {
      if (EC)
        return true;

      if ((EC = it->getSection(sit)))
        return true;

      const Elf_Shdr *section = _objFile->getElfSection(sit);
      const Elf_Sym *symbol = _objFile->getElfSymbol(it);

      StringRef symbolName;
      if ((EC = _objFile->getSymbolName(section, symbol, symbolName)))
        return true;

      if (symbol->st_shndx == llvm::ELF::SHN_ABS) {
        // Create an absolute atom.
        auto *newAtom = new (_readerStorage)
            ELFAbsoluteAtom<ELFT>(*this, symbolName, symbol, symbol->st_value);

        _absoluteAtoms._atoms.push_back(newAtom);
        _symbolToAtomMapping.insert(std::make_pair(symbol, newAtom));
      } else if (symbol->st_shndx == llvm::ELF::SHN_UNDEF) {
        // Create an undefined atom.
        auto *newAtom = new (_readerStorage)
            ELFUndefinedAtom<ELFT>(*this, symbolName, symbol);

        _undefinedAtoms._atoms.push_back(newAtom);
        _symbolToAtomMapping.insert(std::make_pair(symbol, newAtom));
      } else {
        // This is actually a defined symbol. Add it to its section's list of
        // symbols.
        if (symbol->getType() == llvm::ELF::STT_NOTYPE ||
            symbol->getType() == llvm::ELF::STT_OBJECT ||
            symbol->getType() == llvm::ELF::STT_FUNC ||
            symbol->getType() == llvm::ELF::STT_GNU_IFUNC ||
            symbol->getType() == llvm::ELF::STT_SECTION ||
            symbol->getType() == llvm::ELF::STT_FILE ||
            symbol->getType() == llvm::ELF::STT_TLS ||
            symbol->getType() == llvm::ELF::STT_COMMON ||
            symbol->st_shndx == llvm::ELF::SHN_COMMON) {
          _sectionSymbols[section].push_back(symbol);
        } else {
          llvm::errs() << "Unable to create atom for: " << symbolName << "\n";
          EC = llvm::object::object_error::parse_failed;
          return true;
        }
      }
    }
    return false;
  }

  /// \brief Create individual atoms
  bool createAtoms(llvm::error_code &EC) {

    // Cached value of the targetHandler
    TargetHandler<ELFT> &targetHandler =
        _elfTargetInfo.template getTargetHandler<ELFT>();

    for (auto &i : _sectionSymbols) {
      auto &symbols = i.second;
        // Sort symbols by position.
      std::stable_sort(symbols.begin(), symbols.end(),
                       [](const Elf_Sym * A, const Elf_Sym * B) {
        return A->st_value < B->st_value;
      });

      StringRef sectionContents;
      if ((EC = _objFile->getSectionContents(i.first, sectionContents)))
        return true;

      StringRef sectionName;
      if ((EC = _objFile->getSectionName(i.first, sectionName)))
        return true;

      // If the section has no symbols, create a custom atom for it.
      if (i.first->sh_type == llvm::ELF::SHT_PROGBITS && symbols.empty() &&
          !sectionContents.empty()) {
        Elf_Sym *sym = new (_readerStorage) Elf_Sym;
        sym->st_name = 0;
        sym->setBindingAndType(llvm::ELF::STB_LOCAL, llvm::ELF::STT_SECTION);
        sym->st_other = 0;
        sym->st_shndx = 0;
        sym->st_value = 0;
        sym->st_size = 0;
        ArrayRef<uint8_t> content((const uint8_t *)sectionContents.data(),
                                  sectionContents.size());
        auto newAtom = new (_readerStorage) ELFDefinedAtom<ELFT>(
            *this, "", sectionName, sym, i.first, content, 0, 0, _references);
        newAtom->setOrdinal(++_ordinal);
        _definedAtoms._atoms.push_back(newAtom);
        continue;
      }

      ELFDefinedAtom<ELFT> *previous_atom = nullptr;
      // Don't allocate content to a weak symbol, as they may be merged away.
      // Create an anonymous atom to hold the data.
      ELFDefinedAtom<ELFT> *anonAtom = nullptr;
      ELFReference<ELFT> *anonPrecededBy = nullptr;
      ELFReference<ELFT> *anonFollowedBy = nullptr;

      // i.first is the section the symbol lives in
      for (auto si = symbols.begin(), se = symbols.end(); si != se; ++si) {
        StringRef symbolName = "";
        if ((*si)->getType() != llvm::ELF::STT_SECTION)
          if ((EC = _objFile->getSymbolName(i.first, *si, symbolName)))
            return true;

        const Elf_Shdr *section = _objFile->getSection(*si);

        bool isCommon = (*si)->getType() == llvm::ELF::STT_COMMON ||
                        (*si)->st_shndx == llvm::ELF::SHN_COMMON;

        if (isTargetAtom(section, *si)) {
          TargetAtomHandler<ELFT> &_targetAtomHandler =
              targetHandler.targetAtomHandler();
          isCommon =
              ((_targetAtomHandler.getType(*si)) == llvm::ELF::STT_COMMON);
        }

        // Get the symbol's content size
        uint64_t contentSize = 0;
        if (si + 1 == se) {
          // if this is the last symbol, take up the remaining data.
          contentSize = isCommon ? 0 : i.first->sh_size - (*si)->st_value;
        } else {
          contentSize = isCommon ? 0 : (*(si + 1))->st_value - (*si)->st_value;
        }

        // Check to see if we need to add the FollowOn Reference
        // We dont want to do for symbols that are
        // a) common symbols
        ELFReference<ELFT> *followOn = nullptr;
        if (!isCommon && previous_atom) {
          // Replace the followon atom with the anonymous
          // atom that we created, so that the next symbol
          // that we create is a followon from the anonymous
          // atom
          if (!anonFollowedBy) {
            followOn = new (_readerStorage)
                ELFReference<ELFT>(lld::Reference::kindLayoutAfter);
            previous_atom->addReference(followOn);
          }
          else
            followOn = anonFollowedBy;
        }

        // Don't allocate content to a weak symbol, as they may be merged away.
        // Create an anonymous atom to hold the data.
        anonAtom = nullptr;
        anonPrecededBy = nullptr;
        anonFollowedBy = nullptr;
        if ((*si)->getBinding() == llvm::ELF::STB_WEAK && contentSize != 0) {
          // Create a new non-weak ELF symbol.
          auto sym = new (_readerStorage) Elf_Sym;
          *sym = **si;
          sym->setBinding(llvm::ELF::STB_GLOBAL);
          anonAtom = createDefinedAtomAndAssignRelocations(
              "", sectionName, sym, i.first,
              ArrayRef<uint8_t>(
                  (uint8_t *)sectionContents.data() + (*si)->st_value,
                  contentSize));
          anonAtom->setOrdinal(++_ordinal);

          // If this is the last atom, lets not create a followon
          // reference
          if ((si + 1) != se)
            anonFollowedBy = new (_readerStorage)
               ELFReference<ELFT>(lld::Reference::kindLayoutAfter);
          anonPrecededBy = new (_readerStorage)
              ELFReference<ELFT>(lld::Reference::kindLayoutBefore);
          // Add the references to the anonymous atom that we created
          if (anonFollowedBy)
            anonAtom->addReference(anonFollowedBy);
          anonAtom->addReference(anonPrecededBy);
          if (previous_atom)
            anonPrecededBy->setTarget(previous_atom);
          contentSize = 0;
        }

        ArrayRef<uint8_t> symbolData = ArrayRef<uint8_t>(
            (uint8_t *)sectionContents.data() + (*si)->st_value, contentSize);

        // If the linker finds that a section has global atoms that are in a
        // mergeable section, treat them as defined atoms as they shouldnt be
        // merged away as well as these symbols have to be part of symbol
        // resolution
        if (isMergeableSection(section)) {
          if ((*si)->getBinding() == llvm::ELF::STB_GLOBAL) {
            auto definedMergeAtom = new (_readerStorage) ELFDefinedAtom<ELFT>(
                *this, symbolName, sectionName, (*si), section, symbolData,
                _references.size(), _references.size(), _references);
            _definedAtoms._atoms.push_back(definedMergeAtom);
          }
          continue;
        }

        auto newAtom = createDefinedAtomAndAssignRelocations(
            symbolName, sectionName, *si, i.first, symbolData);

        newAtom->setOrdinal(++_ordinal);

        // If the atom was a weak symbol, lets create a followon
        // reference to the anonymous atom that we created
        if ((*si)->getBinding() == llvm::ELF::STB_WEAK && anonAtom) {
          ELFReference<ELFT> *wFollowedBy = new (_readerStorage)
              ELFReference<ELFT>(lld::Reference::kindLayoutAfter);
          wFollowedBy->setTarget(anonAtom);
          newAtom->addReference(wFollowedBy);
        }

        if (followOn) {
          ELFReference<ELFT> *precededby = nullptr;
          // Set the followon atom to the weak atom
          // that we have created, so that they would
          // alias when the file gets written
          if (anonAtom)
            followOn->setTarget(anonAtom);
          else
            followOn->setTarget(newAtom);
          // Add a preceded by reference only if the current atom is not a
          // weak atom
          if ((*si)->getBinding() != llvm::ELF::STB_WEAK) {
            precededby = new (_readerStorage)
                ELFReference<ELFT>(lld::Reference::kindLayoutBefore);
            precededby->setTarget(previous_atom);
            newAtom->addReference(precededby);
          }
        }

        // The previous atom is always the atom created before unless
        // the atom is a weak atom
        if (anonAtom)
          previous_atom = anonAtom;
        else
          previous_atom = newAtom;

        _definedAtoms._atoms.push_back(newAtom);
        _symbolToAtomMapping.insert(std::make_pair((*si), newAtom));
        if (anonAtom)
          _definedAtoms._atoms.push_back(anonAtom);
      }
    }

    updateReferences();
    return false;
  }

  virtual const atom_collection<DefinedAtom> &defined() const {
    return _definedAtoms;
  }

  virtual const atom_collection<UndefinedAtom> &undefined() const {
    return _undefinedAtoms;
  }

  virtual const atom_collection<SharedLibraryAtom> &sharedLibrary() const {
    return _sharedLibraryAtoms;
  }

  virtual const atom_collection<AbsoluteAtom> &absolute() const {
    return _absoluteAtoms;
  }

  virtual const ELFTargetInfo &getTargetInfo() const { return _elfTargetInfo; }

  Atom *findAtom(const Elf_Sym *symbol) {
    return _symbolToAtomMapping.lookup(symbol);
  }

private:

  ELFDefinedAtom<ELFT> *createDefinedAtomAndAssignRelocations(
      StringRef symbolName, StringRef sectionName, const Elf_Sym *symbol,
      const Elf_Shdr *section, ArrayRef<uint8_t> content) {
    unsigned int referenceStart = _references.size();

    // Only relocations that are inside the domain of the atom are added.

    // Add Rela (those with r_addend) references:
    auto rari = _relocationAddendReferences.find(sectionName);
    auto rri = _relocationReferences.find(sectionName);
    if (rari != _relocationAddendReferences.end())
      for (auto &rai : rari->second) {
        if (!((rai.r_offset >= symbol->st_value) &&
              (rai.r_offset < symbol->st_value + content.size())))
          continue;
        bool isMips64EL = _objFile->isMips64EL();
        Reference::Kind kind = (Reference::Kind) rai.getType(isMips64EL);
        uint32_t symbolIndex = rai.getSymbol(isMips64EL);
        auto *ERef = new (_readerStorage)
            ELFReference<ELFT>(&rai, rai.r_offset - symbol->st_value, nullptr,
                               kind, symbolIndex);
        _references.push_back(ERef);
      }

    // Add Rel references.
    if (rri != _relocationReferences.end())
      for (auto &ri : rri->second) {
        if ((ri.r_offset >= symbol->st_value) &&
            (ri.r_offset < symbol->st_value + content.size())) {
          bool isMips64EL = _objFile->isMips64EL();
          Reference::Kind kind = (Reference::Kind) ri.getType(isMips64EL);
          uint32_t symbolIndex = ri.getSymbol(isMips64EL);
          auto *ERef = new (_readerStorage)
              ELFReference<ELFT>(&ri, ri.r_offset - symbol->st_value, nullptr,
                                 kind, symbolIndex);
          // Read the addend from the section contents
          // TODO : We should move the way lld reads relocations totally from
          // ELFObjectFile
          int32_t addend = *(content.data() + ri.r_offset - symbol->st_value);
          ERef->setAddend(addend);
          _references.push_back(ERef);
        }
      }

    // Create the DefinedAtom and add it to the list of DefinedAtoms.
    auto ret = new (_readerStorage) ELFDefinedAtom<
        ELFT>(*this, symbolName, sectionName, symbol, section, content,
              referenceStart, _references.size(), _references);
    ret->permissions();
    ret->contentType();
    return ret;
  }

  /// \brief After all the Atoms and References are created, update each
  /// Reference's target with the Atom pointer it refers to.
  void updateReferences() {

    /// cached value of target relocation handler
    const TargetRelocationHandler<ELFT> &_targetRelocationHandler =
        _elfTargetInfo.template getTargetHandler<ELFT>().getRelocationHandler();

    for (auto &ri : _references) {
      if (ri->kind() >= lld::Reference::kindTargetLow) {
        const Elf_Sym *Symbol = _objFile->getElfSymbol(ri->targetSymbolIndex());
        const Elf_Shdr *shdr = _objFile->getSection(Symbol);
        if (isMergeableSection(shdr)) {
          int64_t relocAddend = _targetRelocationHandler.relocAddend(*ri);
          uint64_t addend = ri->addend() + relocAddend;
          const MergeSectionKey ms(shdr, addend);
          auto msec = _mergedSectionMap.find(ms);
          if (msec == _mergedSectionMap.end()) {
            if (Symbol->getType() != llvm::ELF::STT_SECTION)
              addend = Symbol->st_value + addend;
            MergeAtomsIter mai = findMergeAtom(shdr, addend);
            if (mai != _mergeAtoms.end()) {
              ri->setOffset(addend - ((*mai)->offset()));
              ri->setAddend(0);
              ri->setTarget(*mai);
            } // check
                else
              llvm_unreachable("unable to find a merge atom");
          } // find
              else
            ri->setTarget(msec->second);
        } else
          ri->setTarget(findAtom(Symbol));
      }
    }
  }

  /// \brief Is the atom corresponding to the value of the section and the
  /// symbol a targetAtom ? If so, let the target determine its contentType
  inline bool isTargetAtom(const Elf_Shdr *shdr, const Elf_Sym *sym) {
    if ((shdr && shdr->sh_flags & llvm::ELF::SHF_MASKPROC) ||
        ((sym->st_shndx >= llvm::ELF::SHN_LOPROC) &&
         (sym->st_shndx <= llvm::ELF::SHN_HIPROC)))
      return true;
    return false;
  }

  /// \brief Do we want to ignore the section. Ignored sections are
  /// not processed to create atoms
  bool isIgnoredSection(const Elf_Shdr *section) {
    if (section->sh_size == 0)
      return true;
    switch (section->sh_type) {
    case llvm::ELF::SHT_NOTE:
    case llvm::ELF::SHT_STRTAB:
    case llvm::ELF::SHT_SYMTAB:
    case llvm::ELF::SHT_SYMTAB_SHNDX:
      return true;
    default:
      break;
    }
    return false;
  }

  /// \brief Is the current section be treated as a mergeable string section
  bool isMergeableSection(const Elf_Shdr *section) {
    if (_doStringsMerge && section) {
      int64_t sectionFlags = section->sh_flags;
      sectionFlags &= ~llvm::ELF::SHF_ALLOC;
      // If the section have mergeable strings, the linker would
      // need to split the section into multiple atoms and mark them
      // mergeByContent
      if ((section->sh_entsize < 2) &&
          (sectionFlags == (llvm::ELF::SHF_MERGE | llvm::ELF::SHF_STRINGS))) {
        return true;
      }
    }
    return false;
  }

  llvm::BumpPtrAllocator _readerStorage;
  std::unique_ptr<llvm::object::ELFObjectFile<ELFT> > _objFile;
  atom_collection_vector<DefinedAtom> _definedAtoms;
  atom_collection_vector<UndefinedAtom> _undefinedAtoms;
  atom_collection_vector<SharedLibraryAtom> _sharedLibraryAtoms;
  atom_collection_vector<AbsoluteAtom> _absoluteAtoms;

  /// \brief _relocationAddendReferences and _relocationReferences contain the
  /// list of relocations references.  In ELF, if a section named, ".text" has
  /// relocations will also have a section named ".rel.text" or ".rela.text"
  /// which will hold the entries. -- .rel or .rela is prepended to create
  /// the SHT_REL(A) section name.
  std::unordered_map<
      StringRef,
      range<typename llvm::object::ELFObjectFile<ELFT>::Elf_Rela_Iter> >
  _relocationAddendReferences;
  MergedSectionMapT _mergedSectionMap;
  std::unordered_map<
      StringRef,
      range<typename llvm::object::ELFObjectFile<ELFT>::Elf_Rel_Iter> >
  _relocationReferences;
  std::vector<ELFReference<ELFT> *> _references;
  llvm::DenseMap<const Elf_Sym *, Atom *> _symbolToAtomMapping;
  const ELFTargetInfo &_elfTargetInfo;

  /// \brief Atoms that are created for a section that has the merge property
  /// set
  MergeAtomsT _mergeAtoms;

  /// \brief the section and the symbols that are contained within it to create
  /// used to create atoms
  std::map<const Elf_Shdr *, std::vector<const Elf_Sym *> > _sectionSymbols;

  /// \brief Sections that have merge string property
  std::vector<const Elf_Shdr *> _mergeStringSections;

  int64_t _ordinal;

  /// \brief the cached options relevant while reading the ELF File
  bool _doStringsMerge : 1;
};
} // end namespace elf
} // end namespace lld

#endif