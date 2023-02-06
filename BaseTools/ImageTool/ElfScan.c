/** @file
  Copyright (c) 2022, Mikhail Krichanov. All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "ImageTool.h"

static Elf_Ehdr  *mEhdr        = NULL;
static Elf_Size  mPeAlignment  = 0x0;
static Elf_Addr  mBaseAddress  = 0x0;
static bool      mHasPieRelocs = FALSE;

#if defined (_MSC_EXTENSIONS)
#define EFI_IMAGE_MACHINE_IA32            0x014C
#define EFI_IMAGE_MACHINE_X64             0x8664
#define EFI_IMAGE_MACHINE_ARMTHUMB_MIXED  0x01C2
#define EFI_IMAGE_MACHINE_AARCH64         0xAA64
#endif

extern image_tool_image_info_t mImageInfo;

static
Elf_Shdr *
GetShdrByIndex (
  IN UINT32 Index
  )
{
  UINTN Offset;

  assert (Index < mEhdr->e_shnum);

  Offset = (UINTN)mEhdr->e_shoff + Index * mEhdr->e_shentsize;

  return (Elf_Shdr *)((UINT8 *)mEhdr + Offset);
}

static
char *
GetString (
  IN UINT32 Offset,
  IN UINT32 Index
  )
{
  const Elf_Shdr *Shdr;
  char           *String;

  if (Index == 0) {
    Shdr = GetShdrByIndex (mEhdr->e_shstrndx);
  } else {
    Shdr = GetShdrByIndex (Index);
  }

  if (Offset >= Shdr->sh_size) {
    fprintf (stderr, "ImageTool: Invalid ELF string offset\n");
    return NULL;
  }

  String = (char *)((UINT8 *)mEhdr + Shdr->sh_offset + Offset);

  return String;
}

static
BOOLEAN
IsHiiRsrcShdr (
  IN const Elf_Shdr *Shdr
  )
{
  assert (Shdr != NULL);

  Elf_Shdr *Namedr = GetShdrByIndex (mEhdr->e_shstrndx);

  return (BOOLEAN) (strcmp ((CHAR8*)mEhdr + Namedr->sh_offset + Shdr->sh_name, ELF_HII_SECTION_NAME) == 0);
}

static
BOOLEAN
IsShdrLoadable (
  IN const Elf_Shdr *Shdr
  )
{
  assert (Shdr != NULL);

  return (Shdr->sh_flags & SHF_ALLOC) != 0 && !IsHiiRsrcShdr (Shdr);
}

static
VOID
SetHiiResourceHeader (
  IN OUT UINT8  *Hii,
  IN     UINT32 Offset
  )
{
  UINT32                              Index;
  EFI_IMAGE_RESOURCE_DIRECTORY        *RDir;
  EFI_IMAGE_RESOURCE_DIRECTORY_ENTRY  *RDirEntry;
  EFI_IMAGE_RESOURCE_DIRECTORY_STRING *RDirString;
  EFI_IMAGE_RESOURCE_DATA_ENTRY       *RDataEntry;

  assert (Hii != NULL);

  //
  // Fill Resource section entry
  //
  RDir      = (EFI_IMAGE_RESOURCE_DIRECTORY *)Hii;
  RDirEntry = (EFI_IMAGE_RESOURCE_DIRECTORY_ENTRY *)(RDir + 1);
  for (Index = 0; Index < RDir->NumberOfNamedEntries; ++Index) {
    if (RDirEntry->u1.s.NameIsString) {
      RDirString = (EFI_IMAGE_RESOURCE_DIRECTORY_STRING *)(Hii + RDirEntry->u1.s.NameOffset);

      if ((RDirString->Length == 3)
        && (RDirString->String[0] == L'H')
        && (RDirString->String[1] == L'I')
        && (RDirString->String[2] == L'I')) {
        //
        // Resource Type "HII" found
        //
        if (RDirEntry->u2.s.DataIsDirectory) {
          //
          // Move to next level - resource Name
          //
          RDir      = (EFI_IMAGE_RESOURCE_DIRECTORY *)(Hii + RDirEntry->u2.s.OffsetToDirectory);
          RDirEntry = (EFI_IMAGE_RESOURCE_DIRECTORY_ENTRY *)(RDir + 1);

          if (RDirEntry->u2.s.DataIsDirectory) {
            //
            // Move to next level - resource Language
            //
            RDir      = (EFI_IMAGE_RESOURCE_DIRECTORY *)(Hii + RDirEntry->u2.s.OffsetToDirectory);
            RDirEntry = (EFI_IMAGE_RESOURCE_DIRECTORY_ENTRY *)(RDir + 1);
          }
        }

        //
        // Now it ought to be resource Data. Update its OffsetToData value
        //
        if (!RDirEntry->u2.s.DataIsDirectory) {
          RDataEntry = (EFI_IMAGE_RESOURCE_DATA_ENTRY *)(Hii + RDirEntry->u2.OffsetToData);
          RDataEntry->OffsetToData = RDataEntry->OffsetToData + Offset;
          break;
        }
      }
    }
    RDirEntry++;
  }

  return;
}

static
RETURN_STATUS
ParseElfFile (
  IN const void *File,
  IN uint32_t   FileSize
  )
{
  static const unsigned char Ident[] = {
    ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3, ELFCLASS, ELFDATA2LSB
  };
  const Elf_Shdr *Shdr;
  UINTN          Offset;
  UINT32         Index;
  char           *Last;

  assert (File != NULL || FileSize == 0);

  mEhdr = (Elf_Ehdr *)File;

  //
  // Check header
  //
  if ((FileSize < sizeof (*mEhdr))
    || (memcmp (Ident, mEhdr->e_ident, sizeof (Ident)) != 0)) {
    fprintf (stderr, "ImageTool: Invalid ELF header\n");
    fprintf (stderr, "ImageTool: mEhdr->e_ident[0] = 0x%x expected 0x%x\n", mEhdr->e_ident[0], Ident[0]);
    fprintf (stderr, "ImageTool: mEhdr->e_ident[1] = 0x%x expected 0x%x\n", mEhdr->e_ident[1], Ident[1]);
    fprintf (stderr, "ImageTool: mEhdr->e_ident[2] = 0x%x expected 0x%x\n", mEhdr->e_ident[2], Ident[2]);
    fprintf (stderr, "ImageTool: mEhdr->e_ident[3] = 0x%x expected 0x%x\n", mEhdr->e_ident[3], Ident[3]);
    fprintf (stderr, "ImageTool: mEhdr->e_ident[4] = 0x%x expected 0x%x\n", mEhdr->e_ident[4], Ident[4]);
    fprintf (stderr, "ImageTool: mEhdr->e_ident[5] = 0x%x expected 0x%x\n", mEhdr->e_ident[5], Ident[5]);
    fprintf (stderr, "ImageTool: FileSize = 0x%x  sizeof(*mEhdr) = 0x%lx\n", FileSize, sizeof (*mEhdr));
    return RETURN_UNSUPPORTED;
  }

  if ((mEhdr->e_type != ET_EXEC) && (mEhdr->e_type != ET_DYN)) {
    fprintf (stderr, "ImageTool: ELF e_type not ET_EXEC or ET_DYN\n");
    return RETURN_INCOMPATIBLE_VERSION;
  }

#if defined(EFI_TARGET64)
  if ((mEhdr->e_machine != EM_X86_64) && (mEhdr->e_machine != EM_AARCH64)) {
    fprintf (stderr, "ImageTool: Unsupported ELF e_machine\n");
    return RETURN_INCOMPATIBLE_VERSION;
  }
#elif defined(EFI_TARGET32)
  if ((mEhdr->e_machine != EM_386) && (mEhdr->e_machine != EM_ARM)) {
    fprintf (stderr, "ImageTool: Unsupported ELF e_machine\n");
    return RETURN_INCOMPATIBLE_VERSION;
  }
#endif

  //
  // Check section headers
  //
  mBaseAddress = ~(Elf_Addr)0;
  for (Index = 0; Index < mEhdr->e_shnum; ++Index) {
    Offset = (UINTN)mEhdr->e_shoff + Index * mEhdr->e_shentsize;

    if (FileSize < (Offset + sizeof (*Shdr))) {
      fprintf (stderr, "ImageTool: ELF section header is outside file\n");
      return RETURN_VOLUME_CORRUPTED;
    }

    Shdr = (Elf_Shdr *)((UINT8 *)mEhdr + Offset);

    if ((Shdr->sh_type != SHT_NOBITS)
      && ((FileSize < Shdr->sh_offset) || ((FileSize - Shdr->sh_offset) < Shdr->sh_size))) {
      fprintf (stderr, "ImageTool: ELF section %d points outside file\n", Index);
      return RETURN_VOLUME_CORRUPTED;
    }

    if (Shdr->sh_link >= mEhdr->e_shnum) {
      fprintf (stderr, "ImageTool: ELF %d-th section's sh_link is out of range\n", Index);
      return RETURN_VOLUME_CORRUPTED;
    }

    if ((Shdr->sh_type == SHT_RELA) || (Shdr->sh_type == SHT_REL)) {
      if (Shdr->sh_info >= mEhdr->e_shnum) {
        fprintf (stderr, "ImageTool: ELF %d-th section's sh_info is out of range\n", Index);
        return RETURN_VOLUME_CORRUPTED;
      }

      if (Shdr->sh_info == 0) {
        mHasPieRelocs = TRUE;
      }
    }

    if (Shdr->sh_addr < mBaseAddress) {
      mBaseAddress = Shdr->sh_addr;
    }

    if (!IsShdrLoadable (Shdr)) {
      continue;
    }

    if (mPeAlignment < Shdr->sh_addralign) {
      mPeAlignment = Shdr->sh_addralign;
    }
  }

  if (mEhdr->e_shstrndx >= mEhdr->e_shnum) {
    fprintf (stderr, "ImageTool: Invalid section name string table\n");
    return RETURN_VOLUME_CORRUPTED;
  }
  Shdr = GetShdrByIndex (mEhdr->e_shstrndx);

  if (Shdr->sh_type != SHT_STRTAB) {
    fprintf (stderr, "ImageTool: ELF string table section has wrong type\n");
    return RETURN_VOLUME_CORRUPTED;
  }

  Last = (char *)((UINT8 *)mEhdr + Shdr->sh_offset + Shdr->sh_size - 1);
  if (*Last != '\0') {
    fprintf (stderr, "ImageTool: ELF string table section is not NUL-terminated\n");
    return RETURN_VOLUME_CORRUPTED;
  }

  if ((!IS_POW2(mPeAlignment)) || (mPeAlignment > MAX_PE_ALIGNMENT)) {
    fprintf (stderr, "ImageTool: Invalid section alignment\n");
    return RETURN_VOLUME_CORRUPTED;
  }

  return RETURN_SUCCESS;
}

static
bool
ProcessRelocSection (
  const Elf_Shdr  *Shdr
  )
{
  const Elf_Shdr  *SecShdr;
  //
  // PIE relocations will target dummy section 0.
  //
  if (Shdr->sh_info != 0) {
    //
    // If PIE relocations exist, prefer them.
    //
    if (mHasPieRelocs) {
      return FALSE;
    }
    //
    // Only translate relocations targetting sections that are translated.
    //
    SecShdr = GetShdrByIndex (Shdr->sh_info);

    if (!IsShdrLoadable (SecShdr)) {
      return FALSE;
    }
  } else {
    assert (mHasPieRelocs);
  }

  return TRUE;
}

static
RETURN_STATUS
SetRelocs (
  VOID
  )
{
  UINT32         Index;
  const Elf_Shdr *RelShdr;
  UINTN          RelIdx;
  const Elf_Rela *Rel;
  UINT32         RelNum;
  RelNum = 0;

  for (Index = 0; Index < mEhdr->e_shnum; Index++) {
    RelShdr = GetShdrByIndex (Index);

    if ((RelShdr->sh_type != SHT_REL) && (RelShdr->sh_type != SHT_RELA)) {
      continue;
    }

    if (!ProcessRelocSection (RelShdr)) {
      continue;
    }

    for (RelIdx = 0; RelIdx < RelShdr->sh_size; RelIdx += (UINTN)RelShdr->sh_entsize) {
      Rel = (Elf_Rela *)((UINT8 *)mEhdr + RelShdr->sh_offset + RelIdx);
      //
      // Assume ELF virtual addresses match corresponding PE virtual adresses one to one,
      // so we don't need to recalculate relocations computed by the linker at all r_offset's.
      // We only need to transform ELF relocations' format into PE one.
      //
#if defined(EFI_TARGET64)
      if (mEhdr->e_machine == EM_X86_64) {
        switch (ELF_R_TYPE(Rel->r_info)) {
          case R_X86_64_NONE:
            break;
          case R_X86_64_RELATIVE:
          case R_X86_64_64:
            mImageInfo.RelocInfo.Relocs[RelNum].Type   = EFI_IMAGE_REL_BASED_DIR64;
            mImageInfo.RelocInfo.Relocs[RelNum].Target = (uint32_t)(Rel->r_offset - mBaseAddress);
            ++RelNum;

            break;
          case R_X86_64_32:

            mImageInfo.RelocInfo.Relocs[RelNum].Type   = EFI_IMAGE_REL_BASED_HIGHLOW;
            mImageInfo.RelocInfo.Relocs[RelNum].Target = (uint32_t)(Rel->r_offset - mBaseAddress);
            ++RelNum;

            break;
          case R_X86_64_32S:
          case R_X86_64_PLT32:
          case R_X86_64_PC32:
            break;
          case R_X86_64_GOTPCREL:
          case R_X86_64_GOTPCRELX:
          case R_X86_64_REX_GOTPCRELX:
            //
            // Relocations of these types point to instructions' arguments containing
            // offsets relative to RIP leading to .got entries. As sections' virtual
            // addresses do not change during ELF->PE transform, we don't need to
            // add them to relocations' list. But .got entries contain virtual
            // addresses which must be updated.
            //
            // At r_offset the following value is stored: G + GOT + A - P.
            // To derive .got entry address (G + GOT) compute: value - A + P.
            //
            // Such a method of finding relocatable .got entries can not be used,
            // due to a BUG in clang compiler, which sometimes generates
            // R_X86_64_REX_GOTPCRELX relocations instead of R_X86_64_PC32.
            //
            break;
          default:
            fprintf (stderr, "ImageTool: Unsupported ELF EM_X86_64 relocation 0x%llx in %s\n", ELF_R_TYPE(Rel->r_info), mImageInfo.DebugInfo.SymbolsPath);
            return RETURN_UNSUPPORTED;
        }
      } else if (mEhdr->e_machine == EM_AARCH64) {
        switch (ELF_R_TYPE(Rel->r_info)) {
          case R_AARCH64_NONE0:
          case R_AARCH64_NONE:
          case R_AARCH64_LD64_GOTOFF_LO15:
          case R_AARCH64_LD64_GOTPAGE_LO15:
          case R_AARCH64_LD64_GOT_LO12_NC:
          case R_AARCH64_ADR_GOT_PAGE:
          case R_AARCH64_ADR_PREL_PG_HI21:
          case R_AARCH64_ADD_ABS_LO12_NC:
          case R_AARCH64_LDST8_ABS_LO12_NC:
          case R_AARCH64_LDST16_ABS_LO12_NC:
          case R_AARCH64_LDST32_ABS_LO12_NC:
          case R_AARCH64_LDST64_ABS_LO12_NC:
          case R_AARCH64_LDST128_ABS_LO12_NC:
          case R_AARCH64_ADR_PREL_LO21:
          case R_AARCH64_CONDBR19:
          case R_AARCH64_LD_PREL_LO19:
          case R_AARCH64_CALL26:
          case R_AARCH64_JUMP26:
          case R_AARCH64_PREL64:
          case R_AARCH64_PREL32:
          case R_AARCH64_PREL16:
            break;
          case R_AARCH64_ABS64:
          case R_AARCH64_RELATIVE:
            mImageInfo.RelocInfo.Relocs[RelNum].Type   = EFI_IMAGE_REL_BASED_DIR64;
            mImageInfo.RelocInfo.Relocs[RelNum].Target = (uint32_t)(Rel->r_offset - mBaseAddress);
            ++RelNum;

            break;
          case R_AARCH64_ABS32:

            mImageInfo.RelocInfo.Relocs[RelNum].Type   = EFI_IMAGE_REL_BASED_HIGHLOW;
            mImageInfo.RelocInfo.Relocs[RelNum].Target = (uint32_t)(Rel->r_offset - mBaseAddress);
            ++RelNum;

            break;
          default:
            fprintf (stderr, "ImageTool: Unsupported ELF EM_AARCH64 relocation 0x%llx in %s\n", ELF_R_TYPE(Rel->r_info), mImageInfo.DebugInfo.SymbolsPath);
            return RETURN_UNSUPPORTED;
        }
      }
#elif defined(EFI_TARGET32)
      if (mEhdr->e_machine == EM_386) {
        switch (ELF_R_TYPE(Rel->r_info)) {
          case R_386_NONE:
            break;
          case R_386_32:

            mImageInfo.RelocInfo.Relocs[RelNum].Type   = EFI_IMAGE_REL_BASED_HIGHLOW;
            mImageInfo.RelocInfo.Relocs[RelNum].Target = (Rel->r_offset - mBaseAddress);
            ++RelNum;

            break;
          case R_386_PLT32:
          case R_386_PC32:
            break;
          default:
            fprintf (stderr, "ImageTool: Unsupported ELF EM_386 relocation 0x%x in %s\n", ELF_R_TYPE(Rel->r_info), mImageInfo.DebugInfo.SymbolsPath);
            return RETURN_UNSUPPORTED;
        }
      } else if (mEhdr->e_machine == EM_ARM) {
        switch (ELF32_R_TYPE(Rel->r_info)) {
          case R_ARM_NONE:
          case R_ARM_PC24:
          case R_ARM_REL32:
          case R_ARM_XPC25:
          case R_ARM_THM_PC22:
          case R_ARM_THM_JUMP19:
          case R_ARM_CALL:
          case R_ARM_JMP24:
          case R_ARM_THM_JUMP24:
          case R_ARM_PREL31:
          case R_ARM_MOVW_PREL_NC:
          case R_ARM_MOVT_PREL:
          case R_ARM_THM_MOVW_PREL_NC:
          case R_ARM_THM_MOVT_PREL:
          case R_ARM_THM_JMP6:
          case R_ARM_THM_ALU_PREL_11_0:
          case R_ARM_THM_PC12:
          case R_ARM_REL32_NOI:
          case R_ARM_ALU_PC_G0_NC:
          case R_ARM_ALU_PC_G0:
          case R_ARM_ALU_PC_G1_NC:
          case R_ARM_ALU_PC_G1:
          case R_ARM_ALU_PC_G2:
          case R_ARM_LDR_PC_G1:
          case R_ARM_LDR_PC_G2:
          case R_ARM_LDRS_PC_G0:
          case R_ARM_LDRS_PC_G1:
          case R_ARM_LDRS_PC_G2:
          case R_ARM_LDC_PC_G0:
          case R_ARM_LDC_PC_G1:
          case R_ARM_LDC_PC_G2:
          case R_ARM_THM_JUMP11:
          case R_ARM_THM_JUMP8:
          case R_ARM_TLS_GD32:
          case R_ARM_TLS_LDM32:
          case R_ARM_TLS_IE32:
            break;
          case R_ARM_ABS32:

            mImageInfo.RelocInfo.Relocs[RelNum].Type   = EFI_IMAGE_REL_BASED_HIGHLOW;
            mImageInfo.RelocInfo.Relocs[RelNum].Target = (Rel->r_offset - mBaseAddress);
            ++RelNum;

            break;
          default:
            fprintf (stderr, "ImageTool: Unsupported ELF EM_ARM relocation 0x%x in %s\n", ELF_R_TYPE(Rel->r_info), mImageInfo.DebugInfo.SymbolsPath);
            return RETURN_UNSUPPORTED;
        }
      }
#endif
    }
  }

  mImageInfo.RelocInfo.NumRelocs = RelNum;

  return RETURN_SUCCESS;
}

static
RETURN_STATUS
CreateIntermediate (
  VOID
  )
{
  const Elf_Shdr       *Shdr;
  UINT32               Index;
  image_tool_segment_t *Segments;
  image_tool_reloc_t   *Relocs;
  UINT32               SIndex;
  const Elf_Rel        *Rel;
  UINTN                RIndex;
  char                 *Name;
  UINT32               NumRelocs;

  Segments  = NULL;
  SIndex    = 0;
  Relocs    = NULL;
  NumRelocs = 0;

  for (Index = 0; Index < mEhdr->e_shnum; ++Index) {
    Shdr = GetShdrByIndex (Index);

    if ((Shdr->sh_type == SHT_REL) || (Shdr->sh_type == SHT_RELA)) {
      if ((Shdr->sh_flags & SHF_ALLOC) != 0) {
        fprintf (stderr, "ImageTool: Loadable reloc sections are unsupported\n");
        return RETURN_VOLUME_CORRUPTED;
      }

      if (!ProcessRelocSection (Shdr)) {
        continue;
      }

      for (RIndex = 0; RIndex < Shdr->sh_size; RIndex += (UINTN)Shdr->sh_entsize) {
        Rel = (Elf_Rel *)((UINT8 *)mEhdr + Shdr->sh_offset + RIndex);
#if defined(EFI_TARGET64)
        if (mEhdr->e_machine == EM_X86_64) {
          if ((ELF_R_TYPE(Rel->r_info) == R_X86_64_RELATIVE)
            || (ELF_R_TYPE(Rel->r_info) == R_X86_64_64)
            || (ELF_R_TYPE(Rel->r_info) == R_X86_64_32)
            || (ELF_R_TYPE(Rel->r_info) == R_X86_64_GOTPCREL)
            || (ELF_R_TYPE(Rel->r_info) == R_X86_64_GOTPCRELX)
            || (ELF_R_TYPE(Rel->r_info) == R_X86_64_REX_GOTPCRELX)) {
            ++NumRelocs;
          }
        } else if (mEhdr->e_machine == EM_AARCH64) {
          if ((ELF_R_TYPE(Rel->r_info) == R_AARCH64_ABS64)
            || (ELF_R_TYPE(Rel->r_info) == R_AARCH64_RELATIVE)
            || (ELF_R_TYPE(Rel->r_info) == R_AARCH64_ABS32)) {
            ++NumRelocs;
          }
        }
#elif defined(EFI_TARGET32)
        if (mEhdr->e_machine == EM_386) {
          if (ELF_R_TYPE(Rel->r_info) == R_386_32) {
            ++NumRelocs;
          }
        } else if (mEhdr->e_machine == EM_ARM) {
          if (ELF_R_TYPE(Rel->r_info) == R_ARM_ABS32) {
            ++NumRelocs;
          }
        }
#endif
      }
    }

    if (!IsShdrLoadable (Shdr)) {
      continue;
    }

    ++mImageInfo.SegmentInfo.NumSegments;
  }

  if (mImageInfo.SegmentInfo.NumSegments == 0) {
    fprintf (stderr, "ImageTool: No loadable sections\n");
    return RETURN_VOLUME_CORRUPTED;
  }

  Segments = calloc (1, sizeof (*Segments) * mImageInfo.SegmentInfo.NumSegments);
  if (Segments == NULL) {
    fprintf (stderr, "ImageTool: Could not allocate memory for Segments\n");
    return RETURN_OUT_OF_RESOURCES;
  };

  mImageInfo.SegmentInfo.Segments = Segments;

  if (NumRelocs != 0) {
    Relocs = calloc (1, sizeof (*Relocs) * NumRelocs);
    if (Relocs == NULL) {
      fprintf (stderr, "ImageTool: Could not allocate memory for Relocs\n");
      return RETURN_OUT_OF_RESOURCES;
    };

    mImageInfo.RelocInfo.Relocs = Relocs;
  }

  for (Index = 0; Index < mEhdr->e_shnum; ++Index) {
    Shdr = GetShdrByIndex (Index);

    if ((Shdr->sh_flags & SHF_ALLOC) == 0) {
      continue;
    }

    if (Shdr->sh_type != SHT_PROGBITS && Shdr->sh_type != SHT_NOBITS) {
      fprintf (stderr, "ImageTool: Segment #%d type %x unsupported\n", Index, Shdr->sh_type);
      return RETURN_VOLUME_CORRUPTED;
    }

    if (IsHiiRsrcShdr (Shdr)) {
      mImageInfo.HiiInfo.DataSize = (uint32_t)Shdr->sh_size;

      mImageInfo.HiiInfo.Data = calloc (1, mImageInfo.HiiInfo.DataSize);
      if (mImageInfo.HiiInfo.Data == NULL) {
        fprintf (stderr, "ImageTool: Could not allocate memory for Hii Data\n");
        return RETURN_OUT_OF_RESOURCES;
      };

      if (Shdr->sh_type == SHT_PROGBITS) {
        memcpy (mImageInfo.HiiInfo.Data, (UINT8 *)mEhdr + Shdr->sh_offset, mImageInfo.HiiInfo.DataSize);
      }

      SetHiiResourceHeader (mImageInfo.HiiInfo.Data, (UINT32)(Shdr->sh_addr - mBaseAddress));
    } else {
      Name = GetString (Shdr->sh_name, 0);
      if (Name == NULL) {
        return RETURN_VOLUME_CORRUPTED;
      }

      Segments[SIndex].Name = calloc (1, strlen (Name) + 1);
      if (Segments[SIndex].Name == NULL) {
        fprintf (stderr, "ImageTool: Could not allocate memory for Segment #%d Name\n", Index);
        return RETURN_OUT_OF_RESOURCES;
      };

      memcpy (Segments[SIndex].Name, Name, strlen (Name));

      Segments[SIndex].DataSize = (uint32_t)ALIGN_VALUE (Shdr->sh_size, mPeAlignment);

      Segments[SIndex].Data = calloc (1, Segments[SIndex].DataSize);
      if (Segments[SIndex].Data == NULL) {
        fprintf (stderr, "ImageTool: Could not allocate memory for Segment #%d Data\n", Index);
        return RETURN_OUT_OF_RESOURCES;
      };

      if (Shdr->sh_type == SHT_PROGBITS) {
        memcpy (Segments[SIndex].Data, (UINT8 *)mEhdr + Shdr->sh_offset, (size_t)Shdr->sh_size);
      }

      Segments[SIndex].ImageAddress = Shdr->sh_addr - mBaseAddress;
      Segments[SIndex].ImageSize    = Segments[SIndex].DataSize;
      Segments[SIndex].Read         = true;
      Segments[SIndex].Write        = (Shdr->sh_flags & SHF_WRITE) != 0;
      Segments[SIndex].Execute      = (Shdr->sh_flags & SHF_EXECINSTR) != 0;
      ++SIndex;
    }
  }

  assert (SIndex == mImageInfo.SegmentInfo.NumSegments);

  return SetRelocs();
}

RETURN_STATUS
ScanElf (
  IN const void *File,
  IN uint32_t   FileSize,
  IN const char *SymbolsPath
  )
{
  RETURN_STATUS Status;

  assert (File != NULL || FileSize == 0);

  Status = ParseElfFile (File, FileSize);
  if (RETURN_ERROR (Status)) {
    return Status;
  }

  memset (&mImageInfo, 0, sizeof (mImageInfo));

  mImageInfo.HeaderInfo.BaseAddress       = mBaseAddress;
  mImageInfo.HeaderInfo.EntryPointAddress = (uint32_t)(mEhdr->e_entry - mBaseAddress);
  mImageInfo.HeaderInfo.IsXip             = true;
  mImageInfo.SegmentInfo.SegmentAlignment = (uint32_t)mPeAlignment;
  mImageInfo.RelocInfo.RelocsStripped     = false;
  mImageInfo.DebugInfo.SymbolsPathLen     = strlen (SymbolsPath);

  switch (mEhdr->e_machine) {
#if defined(EFI_TARGET64)
    case EM_X86_64:
      mImageInfo.HeaderInfo.Machine = EFI_IMAGE_MACHINE_X64;
      break;
    case EM_AARCH64:
      mImageInfo.HeaderInfo.Machine = EFI_IMAGE_MACHINE_AARCH64;
      break;
#elif defined(EFI_TARGET32)
    case EM_386:
      mImageInfo.HeaderInfo.Machine = EFI_IMAGE_MACHINE_IA32;
      break;
    case EM_ARM:
      mImageInfo.HeaderInfo.Machine = EFI_IMAGE_MACHINE_ARMTHUMB_MIXED;
      break;
#endif
    default:
      fprintf (stderr, "ImageTool: Unknown ELF architecture %d\n", mEhdr->e_machine);
      return RETURN_INCOMPATIBLE_VERSION;
  }

  mImageInfo.DebugInfo.SymbolsPath = malloc (mImageInfo.DebugInfo.SymbolsPathLen + 1);
  if (mImageInfo.DebugInfo.SymbolsPath == NULL) {
    fprintf (stderr, "ImageTool: Could not allocate memory for Debug Data\n");
    return RETURN_OUT_OF_RESOURCES;
  };

  memmove (mImageInfo.DebugInfo.SymbolsPath, SymbolsPath, mImageInfo.DebugInfo.SymbolsPathLen + 1);

  //
  // There is no corresponding ELF property.
  //
  mImageInfo.HeaderInfo.Subsystem = 0;

  Status = CreateIntermediate ();
  if (RETURN_ERROR (Status)) {
    ToolImageDestruct (&mImageInfo);
  }

  return Status;
}