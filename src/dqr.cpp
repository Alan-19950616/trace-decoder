/*
 * Copyright 2019 Sifive, Inc.
 *
 * dqr.cpp
 */

/*
   This file is part of dqr, the SiFive Inc. Risc-V Nexus 2001 trace decoder.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cassert>

#include "dqr.hpp"
#include "trace.hpp"

//#define DQR_MAXCORES	8

const char * const DQR_VERSION = "0.6";

// static C type helper functions

static int atoh(char a)
{
	if (a >= '0' && a <= '9') {
		return a-'0';
	}

	if (a >= 'a' && a <= 'f') {
		return a-'a'+10;
	}

	if (a >= 'A' && a <= 'F') {
		return a-'A'+10;
	}

	return -1;
}

static void override_print_address(bfd_vma addr, struct disassemble_info *info)
{
//	lookup symbol at addr.

  Disassembler *dp;

  // use field in info to point to disassembler object so we can call member funcs

//  printf("addr: %08x\n",addr);

  dp = (Disassembler *)info->application_data;

  assert(dp != nullptr);

  dp->overridePrintAddress(addr,info);
}

static int asymbol_compare_func(const void *arg1,const void *arg2)
{
	assert(arg1 != nullptr);
	assert(arg2 != nullptr);

	asymbol *first;
	asymbol *second;

	first = *(asymbol **)arg1;
	second = *(asymbol **)arg2;

	// note: use bfd_asymbol_value() because first and second symbols may not be in same section

	return bfd_asymbol_value((asymbol*)first) - bfd_asymbol_value((asymbol*)second);
}

// should probably delete this

__attribute__((unused)) static void dump_syms(asymbol **syms, int num_syms) // @suppress("Unused static function")
{
	assert(syms != nullptr);

    for (int i = 0; i < num_syms; i++) {
    	printf("%d: 0x%llx '%s'",i,bfd_asymbol_value(syms[i]),syms[i]->name);

        if (syms[i]->flags & BSF_LOCAL) {
          printf(" LOCAL");
        }

        if (syms[i]->flags & BSF_GLOBAL) {
          printf(" GLOBAL");
        }

        if (syms[i]->flags & BSF_EXPORT) {
          printf(" EXPORT");
        }

        if (syms[i]->flags & BSF_DEBUGGING) {
          printf(" DEBUGGING");
        }

        if (syms[i]->flags & BSF_FUNCTION) {
          printf(" FUNCTION");
        }

        if (syms[i]->flags & BSF_KEEP) {
          printf(" KEEP");
        }

        if (syms[i]->flags & BSF_ELF_COMMON) {
          printf(" ELF_COMMON");
        }

        if (syms[i]->flags & BSF_WEAK) {
          printf(" WEAK");
        }

        if (syms[i]->flags & BSF_SECTION_SYM) {
          printf(" SECTION_SYM");
        }

        if (syms[i]->flags & BSF_OLD_COMMON) {
          printf(" OLD_COMMON");
        }

        if (syms[i]->flags & BSF_NOT_AT_END) {
          printf(" NOT_AT_END");
        }

        if (syms[i]->flags & BSF_CONSTRUCTOR) {
          printf(" CONSTRUCTOR");
        }

        if (syms[i]->flags & BSF_WARNING) {
          printf(" WARNING");
        }

        if (syms[i]->flags & BSF_INDIRECT) {
          printf(" INDIRECT");
        }

        if (syms[i]->flags & BSF_FILE) {
          printf(" FILE");
        }

        if (syms[i]->flags & BSF_DYNAMIC) {
          printf(" DYNAMIC");
        }

        if (syms[i]->flags & BSF_OBJECT) {
          printf(" OBJECT");
        }

        if (syms[i]->flags & BSF_DEBUGGING_RELOC) {
          printf(" DEBUGGING_RELOC");
        }

        if (syms[i]->flags & BSF_THREAD_LOCAL) {
          printf(" THREAD_LOCAL");
        }

        if (syms[i]->flags & BSF_RELC) {
          printf(" RELC");
        }

        if (syms[i]->flags & BSF_SRELC) {
          printf(" SRELC");
        }

        if (syms[i]->flags & BSF_SYNTHETIC) {
          printf(" SYNTHETIC");
        }

        if (syms[i]->flags & BSF_GNU_INDIRECT_FUNCTION) {
          printf(" GNU_INDIRECT_FUNCTION");
        }

        printf("\n");
    }
}

static char *dis_output;

static int stringify_callback(FILE *stream, const char *format, ...)
{
	char buffer[128];
	va_list vl;
	int rc;

	if (dis_output == nullptr) {
		return 0;
	}

	va_start(vl, format);
	rc = vsprintf(buffer, format, vl);
	va_end(vl);

	strcat(dis_output,buffer);

	return rc;
}

// Section Class Methods

// work with elf file sections using libbfd

section::section()
{
	next      = nullptr;
	abfd      = nullptr;
	asecptr   = nullptr;
	size      = 0;
	startAddr = (TraceDqr::ADDRESS)0;
	endAddr   = (TraceDqr::ADDRESS)0;
	code      = nullptr;
}

section *section::initSection(section **head, asection *newsp)
{
	next = *head;
	*head = this;

	abfd = newsp->owner;
	asecptr = newsp;
	size = newsp->size;
	startAddr = (TraceDqr::ADDRESS)newsp->vma;
	endAddr = (TraceDqr::ADDRESS)(newsp->vma + size - 1);

	int words = (size+1)/2;

	code = nullptr;

//	could make code only read when it is needed (lazzy eval), so if nver used, no memory is
//	ever allocated and no time to read

	code = new (std::nothrow) uint16_t[words];

	assert(code != nullptr);

    bfd_boolean rc;
    rc = bfd_get_section_contents(abfd,newsp,(void*)code,0,size);
    if (rc != TRUE) {
      printf("Error: bfd_get_section_contents() failed\n");
      return nullptr;
    }

    return this;
}

section *section::getSectionByAddress(TraceDqr::ADDRESS addr)
{
	section *sp = this;

	while (sp != nullptr) {
		if ((addr >= sp->startAddr) && (addr <= sp->endAddr)) {
			return sp;
		}

		sp = sp->next;
	}

	return nullptr;
}

int      Instruction::addrSize;
uint32_t Instruction::addrDispFlags;
int      Instruction::addrPrintWidth;

std::string Instruction::addressToString(int labelLevel)
{
	char dst[128];

	addressToText(dst,sizeof dst,labelLevel);

	std::string s = "";

	for (int i = 0; dst[i] != 0; i++) {
		s += dst[i];
	}

	return s;
}

void Instruction::addressToText(char *dst,size_t len,int labelLevel)
{
	assert(dst != nullptr);

	dst[0] = 0;

	if (addrDispFlags & TraceDqr::ADDRDISP_WIDTHAUTO) {
		while (address > (0xffffffffffffffffllu >> (64 - addrPrintWidth*4))) {
			addrPrintWidth += 1;
		}
	}

    int n;

	if ((addrPrintWidth > 8) && (addrDispFlags & TraceDqr::ADDRDISP_SEP)) {
		n = snprintf(dst,len,"%0*x.%08x",addrPrintWidth-8,(uint32_t)(address >> 32),(uint32_t)address);
	}
	else {
		n = snprintf(dst,len,"%0*llx",addrPrintWidth,address);
	}

    if ((labelLevel >= 1) && (addressLabel != nullptr)) {
    	if (addressLabelOffset != 0) {
		    snprintf(dst+n,len-n," <%s+%x>",addressLabel,addressLabelOffset);
		}
		else {
		    snprintf(dst+n,len-n," <%s>",addressLabel);
		}
	}
}

std::string Instruction::instructionToString(int labelLevel)
{
	char dst[128];

	instructionToText(dst,sizeof dst,labelLevel);

	std::string s = "";

	for (int i = 0; dst[i] != 0; i++) {
		s += dst[i];
	}

	return s;
}

void Instruction::instructionToText(char *dst,size_t len,int labelLevel)
{
	assert(dst != nullptr);

	int n;

	dst[0] = 0;

	if (instSize == 32) {
		n = snprintf(dst,len,"%08x           %s",instruction,instructionText);
	}
	else {
		n = snprintf(dst,len,"%04x               %s",instruction,instructionText);
	}

	if (haveOperandAddress) {
		n += snprintf(dst+n,len-n,"%llx",operandAddress);

		if (labelLevel >= 1) {
			if (operandLabel != nullptr) {
				if (operandLabelOffset != 0) {
					snprintf(dst+n,len-n," <%s+%x>",operandLabel,operandLabelOffset);
				}
				else {
					snprintf(dst+n,len-n," <%s>",operandLabel);
				}
			}
		}
	}
}

const char *Source::stripPath(const char *path)
{
	if (path == nullptr) {
		return sourceFile;
	}

	const char *s = sourceFile;

	if (s == nullptr) {
		return nullptr;
	}

	for (;;) {
		if (*path == 0) {
			return s;
		}

		if (tolower(*path) == tolower(*s)) {
			path += 1;
			s += 1;
		}
		else if (*path == '/') {
			if (*s != '\\') {
				return sourceFile;
			}
			path += 1;
			s += 1;
		}
		else if (*path == '\\') {
			if (*s != '/') {
				return sourceFile;
			}
			path += 1;
			s += 1;
		}
		else {
			return sourceFile;
		}
	}

	return nullptr;
}

std::string Source::sourceFileToString(std::string path)
{
	std::string s = "";

	if (sourceFile != nullptr) {
	  const char *sf = stripPath(path.c_str());

	  for (int i = 0; sf[i] != 0; i++) {
		s += sf[i];
	  }
	}

	return s;
}

std::string Source::sourceFileToString()
{
	std::string s = "";

	if (sourceFile != nullptr) {
	  for (int i = 0; sourceFile[i] != 0; i++) {
		s += sourceFile[i];
	  }
	}

	return s;
}

std::string Source::sourceLineToString()
{
	std::string s = "";

	if (sourceLine != nullptr) {
	  for (int i = 0; sourceLine[i] != 0; i++) {
		s += sourceLine[i];
	  }
	}

	return s;
}

std::string Source::sourceFunctionToString()
{
	std::string s = "";

	if (sourceFunction != nullptr) {
	  for (int i = 0; sourceFunction[i] != 0; i++) {
		s += sourceFunction[i];
	  }
	}

	return s;
}

fileReader::fileReader(/*paths*/)
{
	lastFile = nullptr;
	files = nullptr;
}

fileReader::fileList *fileReader::readFile(const char *file)
{
	assert(file != nullptr);

	std::ifstream  f(file, std::ifstream::binary);

	if (!f) {
//		printf("Error: readFile(): could not open file %s for input\n",file);

		// try again after stripping off path

		int l = -1;

		for (int i = 0; file[i] != 0; i++) {
			if ((file[i] == '/') || (file[i] == '\\')) {
				l = i;
			}
		}

		if (l != -1) {
			file = &file[l];
			f.open(file, std::ifstream::binary);
		}
	}

	// could put another try here is !f is true, such as searching a provided path?

	// if (!f) {
	//    char fn[256];
	//
	//    strcpy(fn,path);
	//    if ((path[strlen(path)] != '/') && (path[strlen(path)] != '\\')) {
	//        strcat(fn,'/');
	//    }
	//    strcat(fn,file);	// file is potentially adjusted above]
	//
	//    f.open(fn, ifsteam::binary);
	//  }

	fileList *fl = new fileList;

	fl->next = files;
	fl->funcs = nullptr;
	files = fl;

	int len = strlen(file)+1;
	char *name = new char[len];
	strcpy(name,file);

	fl->name = name;

	if (!f) {
		// always retrun a file list pointer, even if a file isn't found. If file not
		// found, lines will be null

		fl->lineCount = 0;
		fl->lines = nullptr;

		return fl;
	}

	// get length of file:

	f.seekg (0, f.end);
	int length = f.tellg();
	f.seekg (0, f.beg);

	// allocate memory:

	char *buffer = new char [length];

	// read file into buffer

	f.read(buffer,length);

	f.close();

	// count lines

	int lc = 1;

	for (int i = 0; i < length; i++) {
		if (buffer[i] == '\n') {
			lc += 1;
		}
	}

	// create array of line pointers

	char **lines = new char *[lc];

	// initialize arry of ptrs

	int l;
	int s;

	l = 0;
	s = 1;

	for (int i = 0; i < length;i++) {
		if (s != 0) {
			lines[l] = &buffer[i];
			l += 1;
			s = 0;
		}

		// strip out CRs and LFs

		if (buffer[i] == '\r') {
			buffer[i] = 0;
		}
		else if (buffer[i] == '\n') {
			buffer[i] = 0;
			s = 1;
		}
	}

	if (l >= lc) {
		delete [] lines;
		delete [] buffer;

		printf("Error: readFile(): Error computing line count for file\n");

		return nullptr;
	}

	lines[l] = nullptr;

	fl->lineCount = l;
	fl->lines = lines;

	return fl;
}

fileReader::fileList *fileReader::findFile(const char *file)
{
	assert(file != nullptr);

	// first check if file is same as last one uesed

	if (lastFile != nullptr) {
		if (strcmp(lastFile->name,file) == 0) {
			// found file!
			return lastFile;
		}
	}

	struct fileList *fp;

	for (fp = files; fp != nullptr; fp = fp->next) {
		if (strcmp(fp->name,file) == 0) {
			lastFile = fp;
			return fp;
		}
	}

	// didn't find file. See if we can read it in

	fp = readFile(file);

	return fp;
}


Symtab::Symtab(bfd *abfd)
{
	assert(abfd != nullptr);

	this->abfd = abfd;

	if ((bfd_get_file_flags (abfd) & HAS_SYMS)) {
		long storage_needed;

		storage_needed = bfd_get_symtab_upper_bound(abfd);

		if (storage_needed > 0 ) {
			symbol_table = (asymbol**)new (std::nothrow) char[storage_needed];

			assert(symbol_table != nullptr);

			number_of_symbols = bfd_canonicalize_symtab(abfd,symbol_table);

//			printf("symtable 2: storage needed: %d, number of symbols: %d, symbol_table: %08x\n",storage_needed, number_of_symbols, symbol_table);
		}
		else {
			symbol_table = nullptr;
		}
	}
	else {
		symbol_table = nullptr;
	}

    vma = 0;
    index = 0;
}

Symtab::~Symtab()
{
	if (symbol_table != nullptr) {
		delete[] symbol_table;
	}
}

const char *Symtab::getSymbolByAddress(TraceDqr::ADDRESS addr)
{
	if (addr == 0) {
		return nullptr;
	}

	vma = addr;

	struct bfd_section *section;
	bfd_vma section_base_vma;

	index = 0;

	for (int i = 0; i < number_of_symbols; i++) {
		section = symbol_table[i]->section;
	    section_base_vma = section->vma;

//	    if (section_base_vma + symbol_table[i]->value == vma) {
//	    	printf("symabol match for address %p, name: %s\n",vma,symbol_table[i]->name);
////	    	&& (symbol_table[i]->flags & BSF_FUNCTION))
//	    }

	    if ((section_base_vma + symbol_table[i]->value == vma) && (symbol_table[i]->flags & BSF_FUNCTION)) {
	    	index = i;

	    	return symbol_table[i]->name;
	    }
	}

	return nullptr;
}

// probably can delete the getNextSymboByAddress() funcs

const char *Symtab::getNextSymbolByAddress()
{
	struct bfd_section *section;
	bfd_vma section_base_vma;

	for (int i = index+1; i < number_of_symbols; i++) {
		section = symbol_table[i]->section;
	    section_base_vma = section->vma;

	    if ((section_base_vma + symbol_table[i]->value == vma) && (symbol_table[i]->flags & BSF_FUNCTION)) {
	    	index = i;

	    	return symbol_table[i]->name;
	    }
	}

	return nullptr;
}

void Symtab::dump()
{
    printf("number_of_symbols: %ld\n",number_of_symbols);

    for (int i = 0; i < number_of_symbols; i++) {
      printf("symname: %s: \t",symbol_table[i]->name);
      bfd_print_symbol_vandf(abfd,stdout,symbol_table[i]);
      printf("\n");
    }
}

bool ElfReader::init = false;

ElfReader::ElfReader(char *elfname)
{
  assert(elfname != nullptr);

  if (init == false) {
	  // only call bfd_init once - not once per object

	  bfd_init();
	  init = true;
  }

  abfd = bfd_openr(elfname,NULL);
  if (abfd == nullptr) {
    status = TraceDqr::TraceDqr::DQERR_ERR;

    bfd_error_type bfd_error = bfd_get_error();
    printf("Error: bfd_openr() returned null. Error: %d\n",bfd_error);
    printf("Error: Can't open file %s\n",elfname);

    return;
  }

  if(!bfd_check_format(abfd,bfd_object)) {
    printf("Error: ElfReader(): %s not object file: %d\n",elfname,bfd_get_error());

	status = TraceDqr::TraceDqr::DQERR_ERR;

	return;
  }

  const bfd_arch_info_type *aitp;

  aitp = bfd_get_arch_info(abfd);
  if (aitp == nullptr) {
	  printf("Error: ElfReader(): Cannot get arch in for file %s\n",elfname);

	  status = TraceDqr::TraceDqr::DQERR_ERR;
	  return;
  }

// lines below are commented out because the bfd is reaturning the wrong arch for
// riscv arch (returning 67 and not 74)
//
//  printf("arch: %d, bfd_arch_riscv: %d, mach: %d\n",aitp->arch,bfd_arch_riscv,aitp->mach);
//
//  if (aitp->arch != bfd_arch_riscv) {
//    printf("Error: ElfReader(): elf file is not for risc-v architecture\n");
//
//	  status = dqr::dqr::DQERR_ERR;
//	  return;
//  }

  switch (aitp->mach) {
  case bfd_mach_riscv32:
	  archSize = 32;
	  bitsPerWord = aitp->bits_per_word;
	  bitsPerAddress = aitp->bits_per_address;
	  break;
  case bfd_mach_riscv64:
	  archSize = 64;
	  bitsPerWord = aitp->bits_per_word;
	  bitsPerAddress = aitp->bits_per_address;
	  break;
  default:
	  printf("Error: ElfReader(): elf file is for unknown machine type\n");

	  status = TraceDqr::TraceDqr::DQERR_ERR;
	  return;
  }
  symtab = nullptr;
  codeSectionLst = nullptr;

  for (asection *p = abfd->sections; p != NULL; p = p->next) {
	  if (p->flags & SEC_CODE) {
          // found a code section, add to list

		  section *sp = new section;
		  sp->initSection(&codeSectionLst, p);
	  }
  }

  status = TraceDqr::DQERR_OK;
}

ElfReader::~ElfReader()
{
	if (symtab != nullptr) {
		delete symtab;
		symtab = nullptr;
	}

	if (abfd != nullptr) {
		bfd_close(abfd);

		abfd = nullptr;
	}
}

TraceDqr::DQErr ElfReader::getInstructionByAddress(TraceDqr::ADDRESS addr,TraceDqr::RV_INST &inst)
{
	// get instruction at addr

	// Address for code[0] is text->vma

	//don't forget base!!'

	section *sp;
	if (codeSectionLst == nullptr) {
		status = TraceDqr::DQERR_ERR;
		return status;
	}

	sp = codeSectionLst->getSectionByAddress(addr);
	if (sp == nullptr) {
		status = TraceDqr::DQERR_ERR;
		return status;
	}

	if ((addr < sp->startAddr) || (addr > sp->endAddr)) {
		status = TraceDqr::DQERR_ERR;
		return status;
	}

//	if ((addr < text->vma) || (addr >= text->vma + text->size)) {
////			don't know instruction - not part of text segment. need to handle for os
////			if we can't get the instruciton, get the next trace message and try again. or do
////			we need the next sync message and the next trace message and try again?
//
//		// for now, just return an error
//
//		status = dqr::DQERR_ERR;
//		return status;
//	}

	int index;

	index = (addr - sp->startAddr) / 2;

	inst = sp->code[index];

	// if 32 bit instruction, get the second half

	switch (inst & 0x0003) {
	case 0x0000:	// quadrant 0, compressed
	case 0x0001:	// quadrant 1, compressed
	case 0x0002:	// quadrant 2, compressed
		status = TraceDqr::DQERR_OK;
		break;
	case 0x0003:	// not compressed. Assume RV32 for now
		if ((inst & 0x1f) == 0x1f) {
			fprintf(stderr,"Error: getInstructionByAddress(): cann't decode instructions longer than 32 bits\n");
			status = TraceDqr::DQERR_ERR;
			break;
		}

		inst = inst | (((uint32_t)sp->code[index+1]) << 16);

		status = TraceDqr::DQERR_OK;
		break;
	}

	return status;
}

Symtab *ElfReader::getSymtab()
{
	if (symtab == nullptr) {
		symtab = new (std::nothrow) Symtab(abfd);

		assert(symtab != nullptr);
	}

	return symtab;
}

ITCPrint::ITCPrint(int numCores, int buffSize,int channel)
{
	assert((numCores > 0) && (buffSize > 0));

	this->numCores = numCores;
	this->buffSize = buffSize;
	this->printChannel = channel;

	pbuff = new char*[numCores];

	for (int i = 0; i < numCores; i++) {
		pbuff[i] = new char[buffSize];
	}

	numMsgs = new int[numCores];

	for (int i=0; i< numCores; i++) {
		numMsgs[i] = 0;
	}

	pbi = new int[numCores];

	for (int i=0; i< numCores; i++) {
		pbi[i] = 0;
	}

	pbo = new int[numCores];

	for (int i=0; i< numCores; i++) {
		pbo[i] = 0;
	}
}

ITCPrint::~ITCPrint()
{
	if (pbuff != nullptr) {
		for (int i = 0; i < numCores; i++) {
			if (pbuff[i] != nullptr) {
				delete [] pbuff[i];
				pbuff[i] = nullptr;
			}
		}

		delete [] pbuff;
		pbuff = nullptr;
	}

	if (numMsgs != nullptr) {
		delete [] numMsgs;
		numMsgs = nullptr;
	}

	if (pbi != nullptr) {
		delete [] pbi;
		pbi = nullptr;
	}

	if (pbo != nullptr) {
		delete [] pbo;
		pbo = nullptr;
	}
}

int ITCPrint::roomInITCPrintQ(uint8_t core)
{
	if (core >= numCores) {
		return 0;
	}

	if (pbi[core] > pbo[core]) {
		return buffSize - pbi[core] + pbo[core] - 1;
	}

	if (pbi[core] < pbo[core]) {
		return pbo[core] - pbi[core] - 1;
	}

	return buffSize-1;
}

void ITCPrint::print(uint8_t core, uint32_t addr, uint32_t data)
{
	if (core >= numCores) {
		return;
	}

	if ((addr < (uint32_t)printChannel*4) || (addr >= (((uint32_t)printChannel+1)*4))) {
		// not writing to the itcPrint channel

		return;
	}

	char *p = (char *)&data;
	int room = roomInITCPrintQ(core);

	for (int i = 0; ((size_t)i < ((sizeof data) - (addr & 0x03))); i++ ) {
		if (room >= 2) {
			pbuff[core][pbi[core]] = p[i];
			pbi[core] += 1;
			if (pbi[core] >= buffSize) {
				pbi[core] = 0;
			}
			room -= 1;

			switch (p[i]) {
			case 0:
				numMsgs[core] += 1;
				break;
			case '\n':
			case '\r':
				pbuff[core][pbi[core]] = 0;	// add a null termination after the eol
				pbi[core] += 1;
				if (pbi[core] >= buffSize) {
					pbi[core] = 0;
				}
				room -= 1;
				numMsgs[core] += 1;
				break;
			default:
				break;
			}
		}
	}

	pbuff[core][pbi[core]] = 0; // make sure always null terminated
}

void ITCPrint::haveITCPrintData(int numMsgs[], bool havePrintData[])
{
	if (numMsgs != nullptr) {
		for (int i = 0; i < numCores; i++) {
			numMsgs[i] = this->numMsgs[i];
		}
	}

	if (havePrintData != nullptr) {
		for (int i = 0; i < numCores; i++) {
			havePrintData[i] = pbi[i] != pbo[i];
		}
	}
}

bool ITCPrint::getITCPrintMsg(uint8_t core,char* dst,int dstLen)
{
	bool rc = false;

	if (core >= numCores) {
		return false;
	}

	assert((dst != nullptr) && (dstLen > 0));

	if (numMsgs[core] > 0) {
		rc = true;
		numMsgs[core] -= 1;

		while (pbuff[core][pbo[core]] && (dstLen > 1)) {
			*dst++ = pbuff[core][pbo[core]];
			dstLen -= 1;

			pbo[core] += 1;
			if (pbo[core] >= buffSize) {
				pbo[core] = 0;
			}
		}

		*dst = 0;

		// skip past null terminted end of message

		pbo[core] += 1;
		if (pbo[core] >= buffSize) {
			pbo[core] = 0;
		}
	}

	return rc;
}

bool ITCPrint::flushITCPrintMsg(uint8_t core, char *dst, int dstLen)
{
	if (core >= numCores) {
		return false;
	}

	assert((dst != nullptr) && (dstLen > 0));

	if (numMsgs[core] > 0) {
		return getITCPrintMsg(core,dst,dstLen);
	}

	if (pbo[core] != pbi[core]) {
		while (pbuff[core][pbo[core]] && (dstLen > 1)) {
			*dst++ = pbuff[core][pbo[core]];
			dstLen -= 1;

			pbo[core] += 1;
			if (pbo[core] >= buffSize) {
				pbo[core] = 0;
			}
		}
		return true;
	}

	return false;
}

bool ITCPrint::getITCPrintStr(uint8_t core, std::string &s)
{
	if (core >= numCores) {
		return false;
	}

	bool rc = false;

	if (numMsgs[core] > 0) { // stuff in the Q
		rc = true;
		numMsgs[core] -= 1;

		while (pbuff[core][pbo[core]]) {
			s += pbuff[core][pbo[core]];

			pbo[core] += 1;
			if (pbo[core] >= buffSize) {
				pbo[core] = 0;
			}
		}

		pbo[core] += 1;
		if (pbo[core] >= buffSize) {
			pbo[core] = 0;
		}
	}

	return rc;
}

bool ITCPrint::flushITCPrintStr(uint8_t core, std::string &s)
{
	if (core >= numCores) {
		return false;
	}

	if (numMsgs[core] > 0) {
		return getITCPrintStr(core,s);
	}

	if (pbo[core] != pbi[core]) {
		s = "";
		while (pbuff[core][pbo[core]]) {
			s += pbuff[core][pbo[core]];

			pbo[core] += 1;
			if (pbo[core] >= buffSize) {
				pbo[core] = 0;
			}
		}
		return true;
	}

	return false;
}

Analytics::Analytics()
{
	cores = 0;
	num_trace_msgs_all_cores = 0;
	num_trace_bits_all_cores = 0;
	num_trace_bits_all_cores_max = 0;
	num_trace_bits_all_cores_min = 0;
	num_trace_mseo_bits_all_cores = 0;

	num_inst_all_cores = 0;
	num_inst16_all_cores = 0;
	num_inst32_all_cores = 0;

	num_branches_all_cores = 0;

	for (int i = 0; i < DQR_MAXCORES; i++) {
		core[i].num_inst16 = 0;
		core[i].num_inst32 = 0;
		core[i].num_inst = 0;

		core[i].num_trace_msgs = 0;
		core[i].num_trace_syncs = 0;
		core[i].num_trace_dbranch = 0;
		core[i].num_trace_ibranch = 0;
		core[i].num_trace_dataacq = 0;
		core[i].num_trace_dbranchws = 0;
		core[i].num_trace_ibranchws = 0;
		core[i].num_trace_correlation = 0;
		core[i].num_trace_auxaccesswrite = 0;
		core[i].num_trace_ownership = 0;
		core[i].num_trace_error = 0;

		core[i].trace_bits = 0;
		core[i].trace_bits_max = 0;
		core[i].trace_bits_min = 0;
		core[i].trace_bits_mseo = 0;

		core[i].trace_bits_sync = 0;
		core[i].trace_bits_dbranch = 0;
		core[i].trace_bits_ibranch = 0;
		core[i].trace_bits_dataacq = 0;
		core[i].trace_bits_dbranchws = 0;
		core[i].trace_bits_ibranchws = 0;
		core[i].trace_bits_correlation = 0;
		core[i].trace_bits_auxaccesswrite = 0;
		core[i].trace_bits_ownership = 0;
		core[i].trace_bits_error = 0;

		core[i].trace_bits_ts = 0;
		core[i].trace_bits_ts_max = 0;
		core[i].trace_bits_ts_min = 0;

		core[i].trace_bits_uaddr = 0;
		core[i].trace_bits_uaddr_max = 0;
		core[i].trace_bits_uaddr_min = 0;

		core[i].trace_bits_faddr = 0;
		core[i].trace_bits_faddr_max = 0;
		core[i].trace_bits_faddr_min = 0;
	}

	status = TraceDqr::DQERR_OK;
}

TraceDqr::DQErr Analytics::updateTraceInfo(uint32_t core_id,TraceDqr::TCode tcode,uint32_t bits,uint32_t mseo_bits,uint32_t ts_bits,uint32_t addr_bits)
{
	bool have_uaddr = false;
	bool have_faddr = false;

	num_trace_msgs_all_cores += 1;
	num_trace_bits_all_cores += bits;
	num_trace_mseo_bits_all_cores += mseo_bits;

	core[core_id].num_trace_msgs += 1;

	if (bits > num_trace_bits_all_cores_max) {
		num_trace_bits_all_cores_max = bits;
	}

	if ((num_trace_bits_all_cores_min == 0) || (bits < num_trace_bits_all_cores_min)) {
		num_trace_bits_all_cores_min = bits;
	}

	core[core_id].trace_bits_mseo += mseo_bits;
	core[core_id].trace_bits += bits;

	if (bits > core[core_id].trace_bits_max) {
		core[core_id].trace_bits_max = bits;
	}

	if ((core[core_id].trace_bits_min == 0) || (bits < core[core_id].trace_bits_min)) {
		core[core_id].trace_bits_min = bits;
	}

	cores |= (1 << core_id);

	if (ts_bits > 0) {
		core[core_id].num_trace_ts += 1;
		core[core_id].trace_bits_ts += ts_bits;

		if (ts_bits > core[core_id].trace_bits_ts_max) {
			core[core_id].trace_bits_ts_max = ts_bits;
		}

		if ((core[core_id].trace_bits_ts_min == 0) || (ts_bits < core[core_id].trace_bits_ts_min)) {
			core[core_id].trace_bits_ts_min = ts_bits;
		}
	}

	switch (tcode) {
	case TraceDqr::TCODE_OWNERSHIP_TRACE:
		core[core_id].num_trace_ownership += 1;
		core[core_id].trace_bits_ownership += bits;
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH:
		core[core_id].num_trace_dbranch += 1;
		core[core_id].trace_bits_dbranch += bits;
		num_branches_all_cores += 1;
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH:
		core[core_id].num_trace_ibranch += 1;
		core[core_id].trace_bits_ibranch += bits;
		num_branches_all_cores += 1;

		have_uaddr = true;
		break;
	case TraceDqr::TCODE_DATA_ACQUISITION:
		core[core_id].num_trace_dataacq += 1;
		core[core_id].trace_bits_dataacq += bits;
		break;
	case TraceDqr::TCODE_ERROR:
		core[core_id].num_trace_error += 1;
		core[core_id].trace_bits_error += bits;
		break;
	case TraceDqr::TCODE_SYNC:
		core[core_id].num_trace_syncs += 1;
		core[core_id].trace_bits_sync += bits;

		have_faddr = true;
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH_WS:
		core[core_id].num_trace_dbranchws += 1;
		core[core_id].trace_bits_dbranchws += bits;
		num_branches_all_cores += 1;

		have_faddr = true;
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH_WS:
		core[core_id].num_trace_ibranchws += 1;
		core[core_id].trace_bits_ibranchws += bits;
		num_branches_all_cores += 1;

		have_faddr = true;
		break;
	case TraceDqr::TCODE_AUXACCESS_WRITE:
		core[core_id].num_trace_auxaccesswrite += 1;
		core[core_id].trace_bits_auxaccesswrite += bits;
		break;
	case TraceDqr::TCODE_CORRELATION:
		core[core_id].num_trace_correlation += 1;
		core[core_id].trace_bits_ibranchws += bits;
		break;
	case TraceDqr::TCODE_DEBUG_STATUS:
	case TraceDqr::TCODE_DEVICE_ID:
	case TraceDqr::TCODE_DATA_WRITE:
	case TraceDqr::TCODE_DATA_READ:
	case TraceDqr::TCODE_CORRECTION:
	case TraceDqr::TCODE_DATA_WRITE_WS:
	case TraceDqr::TCODE_DATA_READ_WS:
	case TraceDqr::TCODE_WATCHPOINT:
	case TraceDqr::TCODE_OUTPUT_PORTREPLACEMENT:
	case TraceDqr::TCODE_INPUT_PORTREPLACEMENT:
	case TraceDqr::TCODE_AUXACCESS_READ:
	case TraceDqr::TCODE_AUXACCESS_READNEXT:
	case TraceDqr::TCODE_AUXACCESS_WRITENEXT:
	case TraceDqr::TCODE_AUXACCESS_RESPONSE:
	case TraceDqr::TCODE_RESURCEFULL:
	case TraceDqr::TCODE_INDIRECTBRANCHHISOTRY:
	case TraceDqr::TCODE_INDIRECTBRANCHHISORY_WS:
	case TraceDqr::TCODE_REPEATBRANCH:
	case TraceDqr::TCODE_REPEATINSTRUCITON:
	case TraceDqr::TCODE_REPEATSINSTURCIONT_WS:
	case TraceDqr::TCODE_INCIRCUITTRACE:
	default:
		status = TraceDqr::DQERR_ERR;
		return status;
	}

	if (have_uaddr) {
		core[core_id].num_trace_uaddr += 1;
		core[core_id].trace_bits_uaddr += addr_bits;

		if (addr_bits > core[core_id].trace_bits_uaddr_max) {
			core[core_id].trace_bits_uaddr_max = addr_bits;
		}

		if ((core[core_id].trace_bits_uaddr_min == 0) || (addr_bits < core[core_id].trace_bits_uaddr_min)) {
			core[core_id].trace_bits_uaddr_min = addr_bits;
		}
	}
	else if (have_faddr) {
		core[core_id].num_trace_faddr += 1;
		core[core_id].trace_bits_faddr += addr_bits;

		if (addr_bits > core[core_id].trace_bits_faddr_max) {
			core[core_id].trace_bits_faddr_max = addr_bits;
		}

		if ((core[core_id].trace_bits_faddr_min == 0) || (addr_bits < core[core_id].trace_bits_faddr_min)) {
			core[core_id].trace_bits_faddr_min = addr_bits;
		}
	}

	return status;
}

TraceDqr::DQErr Analytics::updateInstructionInfo(uint32_t core_id,uint32_t inst,int instSize)
{
	num_inst_all_cores += 1;
	core[core_id].num_inst += 1;

	switch (instSize) {
	case 16:
		num_inst16_all_cores += 1;
		core[core_id].num_inst16 += 1;
		break;
	case 32:
		num_inst32_all_cores += 1;
		core[core_id].num_inst32 += 1;
		break;
	default:
		status = TraceDqr::DQERR_ERR;
	}

	return status;
}

static void updateDst(int n, char *&dst,int &dst_len)
{
	if (n >= dst_len) {
		dst += dst_len;
		dst_len = 0;
	}
	else {
		dst += n;
		dst_len -= n;
	}
}

void Analytics::toText(char *dst,int dst_len,int detailLevel)
{
	char tmp_dst[512];
	int n;

	assert(dst != nullptr);

	dst[0] = 0;

	if (detailLevel <= 0) {
		return;
	}

	uint32_t have_ts = 0;

	for (int i = 0; i < DQR_MAXCORES; i++) {
		if (cores & (1<<i)) {
			if (core[i].num_trace_ts > 0) {
				have_ts |= (1<<i);
			}
		}
	}

	if (srcBits == 0) {
		n = snprintf(dst,dst_len,"Trace Analytics: Single core");
		updateDst(n,dst,dst_len);
	}
	else {
		n = snprintf(dst,dst_len,"Trace Analytics: Multi core (src field %d bits)",srcBits);
		updateDst(n,dst,dst_len);
	}

	if (have_ts == 0) {
		n = snprintf(dst,dst_len,"; Trace messages do not have timestamps\n");
		updateDst(n,dst,dst_len);
	}
	else if (have_ts == cores) {
		n = snprintf(dst,dst_len,"; Trace messages have timestamps\n");
		updateDst(n,dst,dst_len);
	}
	else {
		n = snprintf(dst,dst_len,"; Some trace messages have timestamps\n");
		updateDst(n,dst,dst_len);
	}

	if (detailLevel == 1) {
		n = snprintf(dst,dst_len,"\n");
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"Instructions             Compressed                   RV32\n");
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"  %10u    %10u (%0.2f%%)    %10u (%0.2f%%)\n",num_inst_all_cores,num_inst16_all_cores,((float)num_inst16_all_cores)/num_inst_all_cores*100.0,num_inst32_all_cores,((float)num_inst32_all_cores)/num_inst_all_cores*100.0);
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"\n");
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"Number of Trace Msgs      Avg Length    Min Length    Max Length    Total Length\n");
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"          %10u          %6.2f    %10u    %10u      %10u\n",num_trace_msgs_all_cores,((float)num_trace_bits_all_cores)/num_trace_msgs_all_cores,num_trace_bits_all_cores_min,num_trace_bits_all_cores_max,num_trace_bits_all_cores);
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"\n");
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"Trace bits per instruction:     %5.2f\n",((float)num_trace_bits_all_cores)/num_inst_all_cores);
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"Instructions per trace message: %5.2f\n",((float)num_inst_all_cores)/num_trace_msgs_all_cores);
		updateDst(n,dst,dst_len);

		n = snprintf(dst,dst_len,"Instructions per taken branch:  %5.2f\n",((float)num_inst_all_cores)/num_branches_all_cores);
		updateDst(n,dst,dst_len);

		if (srcBits > 0) {
			n = snprintf(dst,dst_len,"Src bits %% of message:          %5.2f%%\n",((float)srcBits*num_trace_msgs_all_cores)/num_trace_bits_all_cores*100.0);
			updateDst(n,dst,dst_len);
		}

		if (have_ts != 0 || 1) {
			int bits_ts = 0;

			for (int i = 0; i < DQR_MAXCORES; i++) {
				if (cores & (1<<i)) {
					bits_ts += core[i].trace_bits_ts;
				}
			}
			n = snprintf(dst,dst_len,"Timestamp bits %% of message:    %5.2f%%\n",((float)bits_ts)/num_trace_bits_all_cores*100.0);
			updateDst(n,dst,dst_len);
		}
	}
	else if (detailLevel > 1) {
		int position;
		int tabs[] = {19+21*0,19+21*1,19+21*2,19+21*3,19+21*4,19+21*5,19+21*6,19+21*7,19+21*8};
		uint32_t t1, t2;
		int ts;

		n = sprintf(tmp_dst,"\n");
		n += sprintf(tmp_dst+n,"                 ");

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				n += sprintf(tmp_dst+n,"          Core %d",i);
			}
		}

		if (srcBits > 0) {
			n += sprintf(tmp_dst+n,"               Total");
		}

		n += sprintf(tmp_dst+n,"\n");

		n = snprintf(dst,dst_len,"%s",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Instructions");

		t1 = 0;
		ts = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].num_inst);
				t1 += core[i].num_inst;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  Compressed");

		t2 = 0;
		ts = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_inst16,((float)core[i].num_inst16)/core[i].num_inst*100.0);
				t2 += core[i].num_inst16;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  RV32");

		t2 = 0;
		ts = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_inst32,((float)core[i].num_inst32)/core[i].num_inst*100.0);
				t2 += core[i].num_inst32;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Trace Msgs");

		t1 = 0;
		ts = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].num_trace_msgs);
				t1 += core[i].num_trace_msgs;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += printf(" "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  Sync");

		t2 = 0;
		ts = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_syncs,((float)core[i].num_trace_syncs)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_syncs;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  DBranch");

		t2 = 0;
		ts = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_dbranch,((float)core[i].num_trace_dbranch)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_dbranch;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  IBranch");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_ibranch,((float)core[i].num_trace_ibranch)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_ibranch;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  DBranch WS");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_dbranchws,((float)core[i].num_trace_dbranchws)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_dbranchws;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  IBranch WS");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_ibranchws,((float)core[i].num_trace_ibranchws)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_ibranchws;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  Data Acq");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_dataacq,((float)core[i].num_trace_dataacq)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_dataacq;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  Correlation");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_correlation,((float)core[i].num_trace_correlation)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_correlation;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  Aux Acc Write");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_auxaccesswrite,((float)core[i].num_trace_auxaccesswrite)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_auxaccesswrite;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  Ownership");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_ownership,((float)core[i].num_trace_ownership)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_ownership;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  Error");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",core[i].num_trace_error,((float)core[i].num_trace_error)/core[i].num_trace_msgs*100.0);
				t2 += core[i].num_trace_error;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u (%0.2f%%)",t2,((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Trace Bits Total");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits);
				t1 += core[i].trace_bits;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

//		printf("\n");
//		position = printf("Trace Bits MSEO");
//
//		ts = 0;
//		t1 = 0;
//
//		for (int i = 0; i < DQR_MAXCORES; i++) {
//			if (cores & (1<<i)) {
//				while (position < tabs[ts]) { position += printf(" "); }
//				position += printf("%10u",core[i].trace_bits_mseo);
//				t1 += core[i].trace_bits_mseo;
//				ts += 1;
//			}
//		}
//
//		if (srcBits > 0) {
//			while (position < tabs[ts]) { position += printf(" "); }
//			printf("%10u",t1);
//		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Trace Bits/Inst");

		ts = 0;
		t1 = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%13.2f",((float)core[i].trace_bits)/core[i].num_inst);
				t1 += core[i].trace_bits;
				t2 += core[i].num_inst;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%13.2f",((float)t1)/t2);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Inst/Trace Msg");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%13.2f",((float)core[i].num_inst)/core[i].num_trace_msgs);
				t1 += core[i].num_trace_msgs;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%13.2f",((float)t2)/t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Inst/Taken Branch");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%13.2f",((float)core[i].num_inst)/(core[i].num_trace_dbranch+core[i].num_trace_ibranch+core[i].num_trace_dbranchws+core[i].num_trace_ibranchws));
				t1 += core[i].num_trace_dbranch+core[i].num_trace_ibranch+core[i].num_trace_dbranchws+core[i].num_trace_ibranchws;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%13.2f",((float)t2)/t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Avg Msg Length");

		ts = 0;
		t1 = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%13.2f",((float)core[i].trace_bits)/core[i].num_trace_msgs);
				t1 += core[i].trace_bits;
				t2 += core[i].num_trace_msgs;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%13.2f",((float)t1)/t2);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Min Msg Length");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_min);
				if ((t1 == 0) || (core[i].trace_bits_min < t1)) {
					t1 = core[i].trace_bits_min;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Max Msg Length");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_max);
				if (core[i].trace_bits_max > t1) {
					t1 = core[i].trace_bits_max;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Timestamp Counts");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].num_trace_ts);
				t1 += core[i].num_trace_ts;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  TStamp Size Avg");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				if (core[i].num_trace_ts > 0) {
					position += sprintf(tmp_dst+position,"%13.2f",core[i].trace_bits_ts/core[i].num_trace_ts);
					t2 += core[i].trace_bits_ts;
				}
				else {
					position += sprintf(tmp_dst+position,"         0");
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			if (t1 > 0) {
				position += sprintf(tmp_dst+position,"%13.2f",((float)t2)/t1);
			}
			else {
				position += sprintf(tmp_dst+position,"         0");
			}
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  TStamp Size Min");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_ts_min);
				if ((t1 == 0) || (t1 > core[i].trace_bits_ts_min)) {
					t1 = core[i].trace_bits_ts_min;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  TStamp Size Max");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_ts_max);
				if (core[i].trace_bits_ts_max > t1) {
					t1 = core[i].trace_bits_ts_max;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"Timestamp %% of Msg");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%13.2f%%",((float)core[i].trace_bits_ts)/core[i].trace_bits*100.0);
				t1 += core[i].trace_bits;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%13.2f%%",((float)t2)/t1*100.0);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"UADDR Counts");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].num_trace_uaddr);
				t1 += core[i].num_trace_uaddr;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  UADDR Size Avg");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				if (core[i].num_trace_uaddr > 0) {
					position += sprintf(tmp_dst+position,"%13.2f",((float)core[i].trace_bits_uaddr)/core[i].num_trace_uaddr);
					t2 += core[i].trace_bits_uaddr;
				}
				else {
					position += sprintf(tmp_dst+position,"        0");
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			if (t1 > 0) {
				position += sprintf(tmp_dst+position,"%13.2f",((float)t2)/t1);
			}
			else {
				position += sprintf(tmp_dst+position,"        0");
			}
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  UADDR Size Min");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_uaddr_min);
				if ((t1 == 0) || (core[i].trace_bits_uaddr_min < t1)) {
					t1 = core[i].trace_bits_uaddr_min;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  UADDR Size Max");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_uaddr_max);
				if (t1 < core[i].trace_bits_uaddr_max) {
					t1 = core[i].trace_bits_uaddr_max;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"FADDR Counts");

		ts = 0;
		t1 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].num_trace_faddr);
				t1 += core[i].num_trace_faddr;
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t1);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  FADDR Size Avg");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				if (core[i].num_trace_faddr > 0) {
					position += sprintf(tmp_dst+position,"%13.2f",((float)core[i].trace_bits_faddr)/core[i].num_trace_faddr);
					t2 = core[i].trace_bits_faddr;
				}
				else {
					position += sprintf(tmp_dst+position,"        0");
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			if (t1 > 0) {
				position += sprintf(tmp_dst+position,"%13.2f",((float)t2)/t1);
			}
			else {
				position += sprintf(tmp_dst+position,"        0");
			}
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  FADDR Size Min");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_faddr_min);
				if ((t2 == 0) || (core[i].trace_bits_faddr_min < t2)) {
					t2 = core[i].trace_bits_faddr_min;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t2);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);

		position = sprintf(tmp_dst,"  FADDR Size Max");

		ts = 0;
		t2 = 0;

		for (int i = 0; i < DQR_MAXCORES; i++) {
			if (cores & (1<<i)) {
				while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
				position += sprintf(tmp_dst+position,"%10u",core[i].trace_bits_faddr_max);
				if (core[i].trace_bits_faddr_max > t2) {
					t2 = core[i].trace_bits_faddr_max;
				}
				ts += 1;
			}
		}

		if (srcBits > 0) {
			while (position < tabs[ts]) { position += sprintf(tmp_dst+position," "); }
			position += sprintf(tmp_dst+position,"%10u",t2);
		}

		n = snprintf(dst,dst_len,"%s\n",tmp_dst);
		updateDst(n,dst,dst_len);
	}
}

std::string Analytics::toString(int detailLevel)
{
	char dst[4096];

	dst[0] = 0;

	toText(dst,sizeof dst,detailLevel);

	std::string s = "";

	for (int i = 0; dst[i] != 0; i++) {
		s += dst[i];
	}

	return s;
}

uint32_t NexusMessage::targetFrequency = 0;

NexusMessage::NexusMessage()
{
	msgNum         = 0;
	tcode          = TraceDqr::TCODE_UNDEFINED;
	coreId         = 0;
	haveTimestamp  = false;
	timestamp      = 0;
	currentAddress = 0;
	time           = 0;
}

void NexusMessage::processITCPrintData(ITCPrint *itcPrint)
{
	// need to only do this once per message! Set a flag so we know we have done it.
	// if flag set, return string (or null if none)

	if (itcPrint != nullptr) {
		switch (tcode) {
		case TraceDqr::TCODE_DATA_ACQUISITION:
			itcPrint->print(coreId,dataAcquisition.idTag,dataAcquisition.data);
			break;
		case TraceDqr::TCODE_AUXACCESS_WRITE:
			itcPrint->print(coreId,auxAccessWrite.addr,auxAccessWrite.data);
			break;
		default:
			break;
		}
	}
}

std::string NexusMessage::messageToString(int detailLevel)
{
	char dst[512];

	messageToText(dst,sizeof dst,detailLevel);

	std::string s(dst);

	return s;
}

void  NexusMessage::messageToText(char *dst,size_t dst_len,int level)
{
	assert(dst != nullptr);

	const char *sr;
	const char *bt;
	int n;

	// level = 0, itcprint (always process itc print info
	// level = 1, timestamp + target + itcprint
	// level = 2, message info + timestamp + target + itcprint

//	processITCPrintData();

	if (level <= 0) {
		dst[0] = 0;
		return;
	}

	if (haveTimestamp) {
		if (NexusMessage::targetFrequency != 0) {
			n = snprintf(dst,dst_len,"Msg # %d, time: %0.8f, NxtAddr: %08llx, TCode: ",msgNum,(1.0*time)/NexusMessage::targetFrequency,currentAddress);
		}
		else {
			n = snprintf(dst,dst_len,"Msg # %d, Tics: %lld, NxtAddr: %08llx, TCode: ",msgNum,time,currentAddress);
		}
	}
	else {
		n = snprintf(dst,dst_len,"Msg # %d, NxtAddr: %08llx, TCode: ",msgNum,currentAddress);
	}

	switch (tcode) {
	case TraceDqr::TCODE_DEBUG_STATUS:
		snprintf(dst+n,dst_len-n,"DEBUG STATUS (%d)",tcode);
		break;
	case TraceDqr::TCODE_DEVICE_ID:
		snprintf(dst+n,dst_len-n,"DEVICE ID (%d)",tcode);
		break;
	case TraceDqr::TCODE_OWNERSHIP_TRACE:
		n += snprintf(dst+n,dst_len-n,"OWNERSHIP TRACE (%d)",tcode);

		if (level >= 2) {
			snprintf(dst+n,dst_len-n," process: %d",ownership.process);
		}
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH:
		n += snprintf(dst+n,dst_len-n,"DIRECT BRANCH (%d)",tcode);
		if (level >= 2) {
			snprintf(dst+n,dst_len-n," I-CNT: %d",directBranch.i_cnt);
		}
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH:
		n += snprintf(dst+n,dst_len-n,"INDIRECT BRANCH (%d)",tcode);

		if (level >= 2) {
			switch (indirectBranch.b_type) {
			case TraceDqr::BTYPE_INDIRECT:
				bt = "Indirect";
				break;
			case TraceDqr::BTYPE_EXCEPTION:
				bt = "Exception";
				break;
			case TraceDqr::BTYPE_HARDWARE:
				bt = "Hardware";
				break;
			case TraceDqr::BTYPE_UNDEFINED:
				bt = "Undefined";
				break;
			default:
				bt = "Bad Branch Type";
				break;
			}

			snprintf(dst+n,dst_len-n," Branch Type: %s (%d) I-CNT: %d U-ADDR: 0x%08llx ",bt,indirectBranch.b_type,indirectBranch.i_cnt,indirectBranch.u_addr);
		}
		break;
	case TraceDqr::TCODE_DATA_WRITE:
		snprintf(dst+n,dst_len-n,"DATA WRITE (%d)",tcode);
		break;
	case TraceDqr::TCODE_DATA_READ:
		snprintf(dst+n,dst_len-n,"DATA READ (%d)",tcode);
		break;
	case TraceDqr::TCODE_ERROR:
		n += snprintf(dst+n,dst_len-n,"ERROR (%d)",tcode);
		if (level >= 2) {
			snprintf(dst+n,dst_len-n," Error Type %d",error.etype);
		}
		break;
	case TraceDqr::TCODE_SYNC:
		n += snprintf(dst+n,dst_len-n,"SYNC (%d)",tcode);

		if (level >= 2) {
			switch (sync.sync) {
			case TraceDqr::SYNC_EVTI:
				sr = "EVTI";
				break;
			case TraceDqr::SYNC_EXIT_RESET:
				sr = "Exit Reset";
				break;
			case TraceDqr::SYNC_T_CNT:
				sr = "T Count";
				break;
			case TraceDqr::SYNC_EXIT_DEBUG:
				sr = "Exit Debug";
				break;
			case TraceDqr::SYNC_I_CNT_OVERFLOW:
				sr = "I-Count Overflow";
				break;
			case TraceDqr::SYNC_TRACE_ENABLE:
				sr = "Trace Enable";
				break;
			case TraceDqr::SYNC_WATCHPINT:
				sr = "Watchpoint";
				break;
			case TraceDqr::SYNC_FIFO_OVERRUN:
				sr = "FIFO Overrun";
				break;
			case TraceDqr::SYNC_EXIT_POWERDOWN:
				sr = "Exit Powerdown";
				break;
			case TraceDqr::SYNC_MESSAGE_CONTENTION:
				sr = "Message Contention";
				break;
			case TraceDqr::SYNC_NONE:
				sr = "None";
				break;
			default:
				sr = "Bad Sync Reason";
				break;
			}

			snprintf(dst+n,dst_len-n," Reason: (%d) %s I-CNT: %d F-Addr: 0x%08llx",sync.sync,sr,sync.i_cnt,sync.f_addr);
		}
		break;
	case TraceDqr::TCODE_CORRECTION:
		snprintf(dst+n,dst_len-n,"Correction (%d)",tcode);
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH_WS:
		n += snprintf(dst+n,dst_len-n,"DIRECT BRANCH WS (%d)",tcode);

		if (level >= 2) {
			switch (directBranchWS.sync) {
			case TraceDqr::SYNC_EVTI:
				sr = "EVTI";
				break;
			case TraceDqr::SYNC_EXIT_RESET:
				sr = "Exit Reset";
				break;
			case TraceDqr::SYNC_T_CNT:
				sr = "T Count";
				break;
			case TraceDqr::SYNC_EXIT_DEBUG:
				sr = "Exit Debug";
				break;
			case TraceDqr::SYNC_I_CNT_OVERFLOW:
				sr = "I-Count Overflow";
				break;
			case TraceDqr::SYNC_TRACE_ENABLE:
				sr = "Trace Enable";
				break;
			case TraceDqr::SYNC_WATCHPINT:
				sr = "Watchpoint";
				break;
			case TraceDqr::SYNC_FIFO_OVERRUN:
				sr = "FIFO Overrun";
				break;
			case TraceDqr::SYNC_EXIT_POWERDOWN:
				sr = "Exit Powerdown";
				break;
			case TraceDqr::SYNC_MESSAGE_CONTENTION:
				sr = "Message Contention";
				break;
			case TraceDqr::SYNC_NONE:
				sr = "None";
				break;
			default:
				sr = "Bad Sync Reason";
				break;
			}

			snprintf(dst+n,dst_len-n," Reason: (%d) %s I-CNT: %d F-Addr: 0x%08llx",directBranchWS.sync,sr,directBranchWS.i_cnt,directBranchWS.f_addr);
		}
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH_WS:
		n += snprintf(dst+n,dst_len-n,"INDIRECT BRANCH WS (%d)",tcode);

		if (level >= 2) {
			switch (indirectBranchWS.sync) {
			case TraceDqr::SYNC_EVTI:
				sr = "EVTI";
				break;
			case TraceDqr::SYNC_EXIT_RESET:
				sr = "Exit Reset";
				break;
			case TraceDqr::SYNC_T_CNT:
				sr = "T Count";
				break;
			case TraceDqr::SYNC_EXIT_DEBUG:
				sr = "Exit Debug";
				break;
			case TraceDqr::SYNC_I_CNT_OVERFLOW:
				sr = "I-Count Overflow";
				break;
			case TraceDqr::SYNC_TRACE_ENABLE:
				sr = "Trace Enable";
				break;
			case TraceDqr::SYNC_WATCHPINT:
				sr = "Watchpoint";
				break;
			case TraceDqr::SYNC_FIFO_OVERRUN:
				sr = "FIFO Overrun";
				break;
			case TraceDqr::SYNC_EXIT_POWERDOWN:
				sr = "Exit Powerdown";
				break;
			case TraceDqr::SYNC_MESSAGE_CONTENTION:
				sr = "Message Contention";
				break;
			case TraceDqr::SYNC_NONE:
				sr = "None";
				break;
			default:
				sr = "Bad Sync Reason";
				break;
			}

			switch (indirectBranchWS.b_type) {
			case TraceDqr::BTYPE_INDIRECT:
				bt = "Indirect";
				break;
			case TraceDqr::BTYPE_EXCEPTION:
				bt = "Exception";
				break;
			case TraceDqr::BTYPE_HARDWARE:
				bt = "Hardware";
				break;
			case TraceDqr::BTYPE_UNDEFINED:
				bt = "Undefined";
				break;
			default:
				bt = "Bad Branch Type";
				break;
			}

			snprintf(dst+n,dst_len-n," Reason: (%d) %s Branch Type %s (%d) I-CNT: %d F-Addr: 0x%08llx",indirectBranchWS.sync,sr,bt,indirectBranchWS.b_type,indirectBranchWS.i_cnt,indirectBranchWS.f_addr);
		}
		break;
	case TraceDqr::TCODE_DATA_WRITE_WS:
		snprintf(dst+n,dst_len-n,"DATA WRITE WS (%d)",tcode);
		break;
	case TraceDqr::TCODE_DATA_READ_WS:
		snprintf(dst+n,dst_len-n,"DATA READ WS (%d)",tcode);
		break;
	case TraceDqr::TCODE_WATCHPOINT:
		snprintf(dst+n,dst_len-n,"TCode: WATCHPOINT (%d)",tcode);
		break;
	case TraceDqr::TCODE_OUTPUT_PORTREPLACEMENT:
		snprintf(dst+n,dst_len-n,"OUTPUT PORT REPLACEMENT (%d)",tcode);
		break;
	case TraceDqr::TCODE_INPUT_PORTREPLACEMENT:
		snprintf(dst+n,dst_len-n,"INPUT PORT REPLACEMENT (%d)",tcode);
		break;
	case TraceDqr::TCODE_AUXACCESS_READ:
		snprintf(dst+n,dst_len-n,"AUX ACCESS READ (%d)",tcode);
		break;
	case TraceDqr::TCODE_DATA_ACQUISITION:
		n += snprintf(dst+n,dst_len-n,"DATA ACQUISITION (%d)",tcode);

		if (level >= 2) { // here, if addr not on word boudry, have a partial write!
			switch (dataAcquisition.idTag & 0x03) {
			case 0:
			case 1:
				snprintf(dst+n,dst_len-n," idTag: 0x%08x Data: 0x%08x",dataAcquisition.idTag,dataAcquisition.data);
				break;
			case 2:
				snprintf(dst+n,dst_len-n," idTag: 0x%08x Data: 0x%04x",dataAcquisition.idTag,(uint16_t)dataAcquisition.data);
				break;
			case 3:
				snprintf(dst+n,dst_len-n," idTag: 0x%08x Data: 0x%02x",dataAcquisition.idTag,(uint8_t)dataAcquisition.data);
				break;
			}
		}
		break;
	case TraceDqr::TCODE_AUXACCESS_WRITE:
		n += snprintf(dst+n,dst_len-n,"AUX ACCESS WRITE (%d)",tcode);

		if (level >= 2) { // here, if addr not on word boudry, have a partial write!
			switch (auxAccessWrite.addr & 0x03) {
			case 0:
			case 1:
				snprintf(dst+n,dst_len-n," Addr: 0x%08x Data: 0x%08x",auxAccessWrite.addr,auxAccessWrite.data);
				break;
			case 2:
				snprintf(dst+n,dst_len-n," Addr: 0x%08x Data: 0x%04x",auxAccessWrite.addr,(uint16_t)auxAccessWrite.data);
				break;
			case 3:
				snprintf(dst+n,dst_len-n," Addr: 0x%08x Data: 0x%02x",auxAccessWrite.addr,(uint8_t)auxAccessWrite.data);
				break;
			}
		}
		break;
	case TraceDqr::TCODE_AUXACCESS_READNEXT:
		snprintf(dst+n,dst_len-n,"AUX ACCESS READNEXT (%d)",tcode);
		break;
	case TraceDqr::TCODE_AUXACCESS_WRITENEXT:
		snprintf(dst+n,dst_len-n,"AUX ACCESS WRITENEXT (%d)",tcode);
		break;
	case TraceDqr::TCODE_AUXACCESS_RESPONSE:
		snprintf(dst+n,dst_len-n,"AUXACCESS RESPOINSE (%d)",tcode);
		break;
	case TraceDqr::TCODE_RESURCEFULL:
		snprintf(dst+n,dst_len-n,"RESOURCE FULL (%d)",tcode);
		break;
	case TraceDqr::TCODE_INDIRECTBRANCHHISOTRY:
		snprintf(dst+n,dst_len-n,"INDIRECT BRANCH HISTORY (%d)",tcode);
		break;
	case TraceDqr::TCODE_INDIRECTBRANCHHISORY_WS:
		snprintf(dst+n,dst_len-n,"INDIRECT BRANCH HISTORY WS (%d)",tcode);
		break;
	case TraceDqr::TCODE_REPEATBRANCH:
		snprintf(dst+n,dst_len-n,"REPEAT BRANCH (%d)",tcode);
		break;
	case TraceDqr::TCODE_REPEATINSTRUCITON:
		snprintf(dst+n,dst_len-n,"REPEAT INSTRUCTION (%d)",tcode);
		break;
	case TraceDqr::TCODE_REPEATSINSTURCIONT_WS:
		snprintf(dst+n,dst_len-n,"REPEAT INSTRUCTIN WS (%d)",tcode);
		break;
	case TraceDqr::TCODE_CORRELATION:
		n += snprintf(dst+n,dst_len-n,"CORRELATION (%d)",tcode);

		if (level >= 2) {
			snprintf(dst+n,dst_len-n," EVCODE: %d I-CNT: %d",correlation.evcode,correlation.i_cnt);
		}
		break;
	case TraceDqr::TCODE_INCIRCUITTRACE:
		snprintf(dst+n,dst_len-n,"INCIRCUITTRACE (%d)",tcode);
		break;
	case TraceDqr::TCODE_UNDEFINED:
		snprintf(dst+n,dst_len-n,"UNDEFINED (%d)",tcode);
		break;
	default:
		snprintf(dst+n,dst_len-n,"BAD TCODE (%d)",tcode);
		break;
	}
}

void NexusMessage::dump()
{
	switch (tcode) {
	case TraceDqr::TCODE_DEBUG_STATUS:
		std::cout << "unsupported debug status trace message\n";
		break;
	case TraceDqr::TCODE_DEVICE_ID:
		std::cout << "unsupported device id trace message\n";
		break;
	case TraceDqr::TCODE_OWNERSHIP_TRACE:
//bks
		std::cout << "  # Trace Message(" << msgNum << "): Ownership, process=" << ownership.process; // << ", TS=0x" << hex << timestamp << dec; // << endl;
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH:
//		cout << "(" << traceNum << ") ";
//		cout << "  | Direct Branch | TYPE=DBM, ICNT=" << i_cnt << ", TS=0x" << hex << timestamp << dec; // << endl;
//bks		cout << "  # Trace Message(" << msgNum << "): Direct Branch, ICNT=" << i_cnt; // << ", TS=0x" << hex << timestamp << dec; // << endl;
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH:
//		cout << "(" << traceNum << ") ";
//		cout << "  | Indirect Branch | TYPE=IBM, BTYPE=" << b_type << ", ICNT=" << i_cnt << ", UADDR=0x" << hex << u_addr << ", TS=0x" << timestamp << dec;// << endl;
//bks		cout << "  # Trace Message(" << msgNum << "): Indirect Branch, BTYPE=" << b_type << ", ICNT=" << i_cnt << ", UADDR=0x" << hex << u_addr << dec; // << ", TS=0x" << timestamp << dec;// << endl;
		break;
	case TraceDqr::TCODE_DATA_WRITE:
		std::cout << "unsupported data write trace message\n";
		break;
	case TraceDqr::TCODE_DATA_READ:
		std::cout << "unsupported data read trace message\n";
		break;
	case TraceDqr::TCODE_DATA_ACQUISITION:
		std::cout << "unsupported data acquisition trace message\n";
		break;
	case TraceDqr::TCODE_ERROR:
//bks		cout << "  # Trace Message(" << msgNum << "): Error, ETYPE=" << (uint32_t)etype; // << ", TS=0x" << timestamp << dec;// << endl;
		break;
	case TraceDqr::TCODE_SYNC:
//		cout << "(" << traceNum << "," << syncNum << ") ";
//		cout << "  | Sync | TYPE=SYNC, SYNC=" << sync << ", ICNT=" << i_cnt << ", FADDR=0x" << hex << f_addr << ", TS=0x" << timestamp << dec;// << endl;
//bks		cout << "  # Trace Message(" << msgNum << "): Sync, SYNCREASON=" << sync << ", ICNT=" << i_cnt << ", FADDR=0x" << hex << f_addr << dec; // << ", TS=0x" << timestamp << dec;// << endl;
		break;
	case TraceDqr::TCODE_CORRECTION:
		std::cout << "unsupported correction trace message\n";
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH_WS:
//		cout << "(" << traceNum << "," << syncNum << ") ";
//		cout << "  | Direct Branch With Sync | TYPE=DBWS, SYNC=" << sync << ", ICNT=" << i_cnt << ", FADDR=0x" << hex << f_addr << ", TS=0x" << timestamp << dec;// << endl;
//bks		cout << "  # Trace Message(" << msgNum << "): Direct Branch With Sync, SYNCTYPE=" << sync << ", ICNT=" << i_cnt << ", FADDR=0x" << hex << f_addr << dec; // << ", TS=0x" << timestamp << dec;// << endl;
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH_WS:
//		cout << "(" << traceNum << "," << syncNum << ") ";
//		cout << "  | Indirect Branch With Sync | TYPE=IBWS, SYNC=" << sync << ", BTYPE=" << b_type << ", ICNT=" << i_cnt << ", FADDR=0x" << hex << f_addr << ", TS=0x" << timestamp << dec;// << endl;
//bks		cout << "  # Trace Message(" << msgNum << "): Indirect Branch With sync, SYNCTYPE=" << sync << ", BTYPE=" << b_type << ", ICNT=" << i_cnt << ", FADDR=0x" << hex << f_addr << dec; // << ", TS=0x" << timestamp << dec;// << endl;
		break;
	case TraceDqr::TCODE_DATA_WRITE_WS:
		std::cout << "unsupported data write with sync trace message\n";
		break;
	case TraceDqr::TCODE_DATA_READ_WS:
		std::cout << "unsupported data read with sync trace message\n";
		break;
	case TraceDqr::TCODE_WATCHPOINT:
		std::cout << "unsupported watchpoint trace message\n";
		break;
	case TraceDqr::TCODE_AUXACCESS_WRITE:
//bks		cout << "  # Trace Message(" << msgNum << "): Auxillary Access Write, address=" << hex << auxAddr << dec << ", data=" << hex << data << dec; // << ", TS=0x" << hex << timestamp << dec; // << endl;
		break;
	case TraceDqr::TCODE_CORRELATION:
//bks		cout << "  # Trace Message(" << msgNum << "): Correlation, EVCODE=" << (uint32_t)evcode << ", CDF=" << (int)cdf << ", ICNT=" << i_cnt << "\n"; // << ", TS=0x" << timestamp << dec;// << endl;
		break;
	default:
		std::cout << "Error: NexusMessage::dump(): Unknown TCODE " << std::hex << tcode << std::dec << "msgnum: " << msgNum << std::endl;
	}
}

SliceFileParser::SliceFileParser(char *filename, bool binary, int srcBits)
{
	assert(filename != nullptr);

	srcbits = srcBits;

	firstMsg = true;

	msgSlices      = 0;
	bitIndex       = 0;
	currentAddress = 0;
	currentTime    = 0;

	this->binary = binary;

	if (binary) {
		tf.open(filename, std::ios::in | std::ios::binary);
	}
	else {
		tf.open(filename, std::ios::in);
	}

	if (!tf) {
		printf("Error: SliceFileParder(): could not open file %s for input",filename);
		status = TraceDqr::DQERR_OPEN;
	}
	else {
		status = TraceDqr::DQERR_OK;
	}

#if	0
	// read entire slice file, create multiple quese base on src field

	readAllTraceMsgs();
#endif
}

SliceFileParser::~SliceFileParser()
{
	if (tf.is_open()) {
		tf.close();
	}
}

void SliceFileParser::dump()
{
	//msg and msgSlices

	for (int i = 0; i < msgSlices; i++) {
		std::cout << std::setw(2) << i+1;
		std::cout << " | ";
		std::cout << std::setfill('0');
		std::cout << std::setw(2) << std::hex << int(msg[i] >> 2) << std::dec;
		std::cout << std::setfill(' ');
		std::cout << " | ";
		std::cout << int((msg[i] >> 1) & 1);
		std::cout << " ";
		std::cout << int(msg[i] & 1);
		std::cout << std::endl;
	}
}

TraceDqr::DQErr SliceFileParser::parseDirectBranch(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;	// start bits at 6 to account for tcode
	int        ts_bits = 0;

	nm.tcode = TraceDqr::TCODE_DIRECT_BRANCH;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the variable length the i-cnt

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

    nm.directBranch.i_cnt = (int)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		bits += width;
		ts_bits = width;

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_DIRECT_BRANCH,bits+msgSlices*2,msgSlices*2,ts_bits,0);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseDirectBranchWS(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;
	int        addr_bits;

	nm.tcode = TraceDqr::TCODE_DIRECT_BRANCH_WS;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the fixed length sync reason field

	rc = parseFixedField(4,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 4;

	nm.directBranchWS.sync   = (TraceDqr::SyncReason)tmp;

	// parse the variable length the i-cnt

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.directBranchWS.i_cnt  = (int)tmp;

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;
	addr_bits = width;

	nm.directBranchWS.f_addr = (TraceDqr::ADDRESS)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		bits += width;
		ts_bits = width;

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_DIRECT_BRANCH_WS,bits+msgSlices*2,msgSlices*2,ts_bits,addr_bits);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseIndirectBranch(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;
	int        addr_bits;

	nm.tcode = TraceDqr::TCODE_INDIRECT_BRANCH;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the fixed lenght b-type

	rc = parseFixedField(2,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 2;

	nm.indirectBranch.b_type = (TraceDqr::BType)tmp;

	// parse the variable length the i-cnt

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.indirectBranch.i_cnt  = (int)tmp;

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;
	addr_bits = width;

	nm.indirectBranch.u_addr = (TraceDqr::ADDRESS)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		bits += width;
		ts_bits = width;

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_INDIRECT_BRANCH,bits+msgSlices*2,msgSlices*2,ts_bits,addr_bits);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseIndirectBranchWS(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;
	int        addr_bits;

	nm.tcode = TraceDqr::TCODE_INDIRECT_BRANCH_WS;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the fixed length sync field

	rc = parseFixedField(4,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 4;

	nm.indirectBranchWS.sync = (TraceDqr::SyncReason)tmp;

	// parse the fixed length b-type

	rc = parseFixedField(2,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 2;

	nm.indirectBranchWS.b_type = (TraceDqr::BType)tmp;

	// parse the variable length the i-cnt

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.indirectBranchWS.i_cnt = (int)tmp;

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;
	addr_bits = width;

	nm.indirectBranchWS.f_addr = (TraceDqr::ADDRESS)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		bits += width;
		ts_bits = width;

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_INDIRECT_BRANCH_WS,bits+msgSlices*2,msgSlices*2,ts_bits,addr_bits);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseSync(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;
	int        addr_bits;

	nm.tcode         = TraceDqr::TCODE_SYNC;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the variable length the sync

	rc = parseFixedField(4,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 4;

	nm.sync.sync     = (TraceDqr::SyncReason)tmp;

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.sync.i_cnt    = (int)tmp;

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;
	addr_bits = width;

	nm.sync.f_addr   = (TraceDqr::ADDRESS)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		bits += width;
		ts_bits = width;

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_SYNC,bits+msgSlices*2,msgSlices*2,ts_bits,addr_bits);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseCorrelation(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;

	nm.tcode = TraceDqr::TCODE_CORRELATION;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the 4-bit evcode field

	rc = parseFixedField(4,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 4;

	nm.correlation.evcode = (uint8_t)tmp;

	// parse the 2-bit cdf field.

	rc = parseFixedField(2,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 2;

	if (tmp != 0) {
		printf("Error: DQErr SliceFileParser::parseCorrelation(): Expected CDF to be 0 (%d)\n",(int)tmp);

		status = TraceDqr::DQERR_ERR;

		return status;
	}

	nm.correlation.cdf = (uint8_t)tmp;

	// parse the variable length i-cnt field

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.correlation.i_cnt = (int)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		bits += width;
		ts_bits = width;

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_CORRELATION,bits+msgSlices*2,msgSlices*2,ts_bits,0);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseError(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;

	nm.tcode = TraceDqr::TCODE_ERROR;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the 4 bit ETYPE field

	rc = parseFixedField(4,&tmp);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += 4;

	nm.error.etype = (uint8_t)tmp;

	// parse the variable sized padd field

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		bits += width;
		ts_bits = width;

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_ERROR,bits+msgSlices*2,msgSlices*2,ts_bits,0);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseOwnershipTrace(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;

	nm.tcode = TraceDqr::TCODE_OWNERSHIP_TRACE;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the variable length process ID field

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.ownership.process = (int)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		bits += width;
		ts_bits = width;

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_OWNERSHIP_TRACE,bits+msgSlices*2,msgSlices*2,ts_bits,0);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseAuxAccessWrite(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;

	nm.tcode = TraceDqr::TCODE_AUXACCESS_WRITE;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the ADDR field

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.auxAccessWrite.addr = (uint32_t)tmp;

	// parse the data field

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.auxAccessWrite.data = (uint32_t)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		bits += width;
		ts_bits = width;

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_AUXACCESS_WRITE,bits+msgSlices*2,msgSlices*2,ts_bits,0);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseDataAcquisition(NexusMessage &nm,Analytics &analytics)
{
	TraceDqr::DQErr rc;
	uint64_t   tmp;
	int        width;
	int        bits = 6;
	int        ts_bits = 0;

	nm.tcode = TraceDqr::TCODE_DATA_ACQUISITION;

	// if multicore, parse src field

	if (srcbits > 0) {
        rc = parseFixedField(srcbits,&tmp);
        if (rc != TraceDqr::DQERR_OK) {
            status = rc;

            return status;
        }

        bits += srcbits;

        nm.coreId = (uint8_t)tmp;
	}
	else {
		nm.coreId = 0;
	}

	// parse the idTag field

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.dataAcquisition.idTag = (uint32_t)tmp;

	// parse the data field

	rc = parseVarField(&tmp,&width);
	if (rc != TraceDqr::DQERR_OK) {
		status = rc;

		return status;
	}

	bits += width;

	nm.dataAcquisition.data = (uint32_t)tmp;

	if (eom == true) {
		nm.haveTimestamp = false;
		nm.timestamp = 0;
	}
	else {
		rc = parseVarField(&tmp,&width); // this field is optional - check err
		if (rc != TraceDqr::DQERR_OK) {
			status = rc;

			return status;
		}

		// check if entire message has been consumed

		if (eom != true) {
			status = TraceDqr::DQERR_BM;

			return status;
		}

		bits += width;
		ts_bits = width;

		nm.haveTimestamp = true;
		nm.timestamp = (TraceDqr::TIMESTAMP)tmp;
	}

	status = analytics.updateTraceInfo(nm.coreId,TraceDqr::TCODE_DATA_ACQUISITION,bits+msgSlices*2,msgSlices*2,ts_bits,0);

	nm.msgNum = analytics.currentTraceMsgNum();

	return status;
}

TraceDqr::DQErr SliceFileParser::parseFixedField(int width, uint64_t *val)
{
	assert(width != 0);
	assert(val != nullptr);

	uint64_t tmp_val = 0;

	int i;
	int b;

	i = bitIndex / 6;
	b = bitIndex % 6;

//	printf("parseFixedField(): bitIndex:%d, i:%d, b:%d, width: %d\n",bitIndex,i,b,width);

	bitIndex += width;

	// for read error checking we should make sure that the MSEO bits are
	// correct for this field. But for now, we don't

	if (bitIndex >= msgSlices * 6) {
		// read past end of message

		status = TraceDqr::DQERR_EOM;

		return TraceDqr::DQERR_EOM;
	}

	if (b+width > 6) {
		// fixed field crossed byte boundry - get the bits at msg[i]

		// work from lsb's to msb's

		tmp_val = (uint64_t)(msg[i] >> (b+2));	// add 2 because of the mseo bits

		int consumed = 6-b;
		int remainingWidth = width - consumed;

		i += 1;

//		b = 0; comment this line out because we jsut don't use b anymore. It is 0 for the rest of the call

		// get the middle bits

		while (remainingWidth >= 6) {
			tmp_val |= ((uint64_t)(msg[i] >> 2)) << consumed;
			i += 1;
			remainingWidth -= 6;
			consumed += 6;
		}

		// now get the last bits

		if (remainingWidth > 0) {
			tmp_val |= ((uint64_t)(((uint8_t)(msg[i] << (6-remainingWidth))) >> (6-remainingWidth+2))) << consumed;
		}

		*val = tmp_val;
	}
	else {
		uint8_t v;

		// strip off upper and lower bits not part of field

		v = msg[i] << (6-(b+width));
		v = v >> ((6-(b+width))+b+2);

		*val = uint64_t(v);
	}

	if ((msg[i] & 0x03) == TraceDqr::MSEO_END) {
		eom = true;
	}

	return TraceDqr::DQERR_OK;
}

TraceDqr::DQErr SliceFileParser::parseVarField(uint64_t *val,int *width)
{
	assert(val != nullptr);

	int i;
	int b;
	int w = 0;

	i = bitIndex / 6;
	b = bitIndex % 6;

	if (i >= msgSlices) {
		// read past end of message

		status = TraceDqr::DQERR_EOM;

		return TraceDqr::DQERR_EOM;
	}

	uint64_t v;

	// strip off upper and lower bits not part of field

	w = 6-b;

//	printf("parseVarField(): bitIndex:%d, i:%d, b:%d, width: %d\n",bitIndex,i,b,width);

	v = msg[i] >> (b+2);

	while ((msg[i] & 0x03) == TraceDqr::MSEO_NORMAL) {
		i += 1;
		if (i >= msgSlices) {
			// read past end of message

			status = TraceDqr::DQERR_ERR;

			return TraceDqr::DQERR_ERR;
		}

		v = v | ((msg[i] >> 2) << w);
		w += 6;
	}

	if ((msg[i] & 0x03) == TraceDqr::MSEO_END) {
		eom = true;
	}

	bitIndex += w;

	*width = w;
	*val = v;

//	printf("-> bitIndex: %d, value: %llx\n",bitIndex,v);

	return TraceDqr::DQERR_OK;
}

TraceDqr::DQErr SliceFileParser::readBinaryMsg()
{
	// start by stripping off end of message or end of var bytes. These would be here in the case
	// of a wrapped buffer, or some kind of corruption

	do {
		tf.read((char*)&msg[0],sizeof msg[0]);
		if (!tf) {
			if (tf.eof()) {
				status = TraceDqr::DQERR_EOF;

				std::cout << "Info: End of trace file\n";
			}
			else {
				status = TraceDqr::DQERR_ERR;

				std::cout << "Error reading trace file\n";
			}

			tf.close();

			return status;
		}
		if (((msg[0] & 0x3) != TraceDqr::MSEO_NORMAL) && ((msg[0] & 0x3) != TraceDqr::MSEO_END)) {
			printf("skipping: %02x\n",msg[0]);
		}
	} while ((msg[0] & 0x3) != TraceDqr::MSEO_NORMAL);

	// make sure this is start of nexus message

	if ((msg[0] & 0x3) != TraceDqr::MSEO_NORMAL) {

		tf.close();

		status = TraceDqr::DQERR_ERR;

		std::cout << "Error: SliceFileParser::readBinaryMsg(): expected start of message; got" << std::hex << static_cast<uint8_t>(msg[0] & 0x3) << std::dec << std::endl;

		return TraceDqr::DQERR_ERR;
	}

	bool done = false;

	for (int i = 1; !done; i++) {
		if (i >= (int)(sizeof msg / sizeof msg[0])) {

			tf.close();

			std::cout << "Error: SliceFileParser::readBinaryMsg(): msg buffer overflow" << std::endl;

			status = TraceDqr::DQERR_ERR;

			return TraceDqr::DQERR_ERR;
		}

		tf.read((char*)&msg[i],sizeof msg[0]);
		if (!tf) {
			if (tf.eof()) {
				status = TraceDqr::DQERR_EOF;

				std::cout << "End of trace file\n";
			}
			else {
				status = TraceDqr::DQERR_ERR;

				std::cout << "Error reading trace filem\n";
			}

			tf.close();

			return status;
		}

		if ((msg[i] & 0x03) == TraceDqr::MSEO_END) {
			done = true;
			msgSlices = i+1;
		}
	}

	eom = false;
	bitIndex = 0;

	return TraceDqr::DQERR_OK;
}

TraceDqr::DQErr SliceFileParser::readNextByte(uint8_t *byte)
{
	char c;

	// strip white space, comments, cr, and lf

	enum {
		STRIPPING_WHITESPACE,
		STRIPPING_COMMENT,
		STRIPPING_DONE
	} ss = STRIPPING_WHITESPACE;

	do {
		tf.read((char*)&c,sizeof c);
		if (!tf) {
			tf.close();

			status = TraceDqr::DQERR_EOF;

			return TraceDqr::DQERR_EOF;
		}

		switch (ss) {
		case STRIPPING_WHITESPACE:
			switch (c) {
			case '#':
				ss = STRIPPING_COMMENT;
				break;
			case '\n':
			case '\r':
			case ' ':
			case '\t':
				break;
			default:
				ss = STRIPPING_DONE;
			}
			break;
		case STRIPPING_COMMENT:
			switch (c) {
			case '\n':
				ss = STRIPPING_WHITESPACE;
				break;
			}
			break;
		default:
			break;
		}
	} while (ss != STRIPPING_DONE);

	// now try to get two hex digits

	tf.read((char*)&c,sizeof c);
	if (!tf) {
		tf.close();

		status = TraceDqr::DQERR_EOF;

		return TraceDqr::DQERR_EOF;
	}

	int hd;
	uint8_t hn;

	hd = atoh(c);
	if (hd < 0) {
		status = TraceDqr::DQERR_ERR;

		return TraceDqr::DQERR_ERR;
	}

	hn = hd << 4;

	hd = atoh(c);
	if (hd < 0) {
		status = TraceDqr::DQERR_ERR;

		return TraceDqr::DQERR_ERR;
	}

	hn = hn | hd;

	*byte = hn;

	return TraceDqr::DQERR_OK;
}

TraceDqr::DQErr SliceFileParser::readAscMsg()
{
	std::cout << "Error: SliceFileP{arser::readAscMsg(): not implemented" << std::endl;
	status = TraceDqr::DQERR_ERR;

	return TraceDqr::DQERR_ERR;

	// strip off idle bytes

	do {
		if (readNextByte(&msg[0]) != TraceDqr::DQERR_OK) {
			return status;
		}
	} while ((msg[0] & 0x3) == TraceDqr::MSEO_END);

	// make sure this is start of nexus message

	if ((msg[0] & 0x3) != TraceDqr::MSEO_NORMAL) {
		status = TraceDqr::DQERR_ERR;

		std::cout << "Error: SliceFileParser::readBinaryMsg(): expected start of message; got" << std::hex << static_cast<uint8_t>(msg[0] & 0x3) << std::dec << std::endl;

		return TraceDqr::DQERR_ERR;
	}

	// now get bytes

	bool done = false;

	for (int i = 1; !done; i++) {
		if (i >= (int)(sizeof msg / sizeof msg[0])) {
			tf.close();

			std::cout << "Error: SliceFileParser::readBinaryMsg(): msg buffer overflow" << std::endl;

			status = TraceDqr::DQERR_ERR;

			return TraceDqr::DQERR_ERR;
		}

		if (readNextByte(&msg[i]) != TraceDqr::DQERR_OK) {
			return status;
		}

		if ((msg[i] & 0x03) == TraceDqr::MSEO_END) {
			done = true;
			msgSlices = i+1;
		}
	}

	eom = false;

	bitIndex = 0;

	return TraceDqr::DQERR_OK;
}

TraceDqr::DQErr SliceFileParser::readNextTraceMsg(NexusMessage &nm,Analytics &analytics)	// generator to read trace messages one at a time
{
	assert(status == TraceDqr::DQERR_OK);

	TraceDqr::DQErr rc;
	uint64_t   val;
	uint8_t    tcode;

	// read from file, store in object, compute and fill out full fields, such as address and more later
	// need some place to put it. An object

//	int i = 1;
//	do {
//		printf("trace msg %d\n",i);
//		i += 1;
//		rc = readBinaryMsg();
//		printf("rc: %d\n",rc);
//		dump();
//	} while (rc == dqr::DQERR_OK); // foo

	if (binary) {
		rc = readBinaryMsg();
		if (rc != TraceDqr::DQERR_OK) {
			if (rc != TraceDqr::DQERR_EOF) {
				std::cout << "Error: (): readBinaryMsg() returned error " << rc << std::endl;
			}

			status = rc;

			return status;
		}
	}
	else {	// text trace messages
		rc = readAscMsg();
		if (rc != TraceDqr::DQERR_OK) {
			if (rc != TraceDqr::DQERR_EOF) {
				std::cout << "Error: (): read/TxtMsg() returned error " << rc << std::endl;
			}

			status = rc;

			return status;
		}
	}

// crow	dump();

	rc = parseFixedField(6, &val);

	if (rc != TraceDqr::DQERR_OK) {
		std::cout << "Error: (): could not read tcode\n";

		status = rc;

		return status;
	}

	tcode = (uint8_t)val;

	switch (tcode) {
	case TraceDqr::TCODE_DEBUG_STATUS:
		std::cout << "unsupported debug status trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_DEVICE_ID:
		std::cout << "unsupported device id trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_OWNERSHIP_TRACE:
		status = parseOwnershipTrace(nm,analytics);
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH:
//		cout << " | direct branch";
		status = parseDirectBranch(nm,analytics);
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH:
//		cout << "| indirect branch";
		status = parseIndirectBranch(nm,analytics);
		break;
	case TraceDqr::TCODE_DATA_WRITE:
		std::cout << "unsupported data write trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_DATA_READ:
		std::cout << "unsupported data read trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_DATA_ACQUISITION:
		status = parseDataAcquisition(nm,analytics);
		break;
	case TraceDqr::TCODE_ERROR:
		status = parseError(nm,analytics);
		break;
	case TraceDqr::TCODE_SYNC:
//		cout << "| sync";
		status = parseSync(nm,analytics);
		break;
	case TraceDqr::TCODE_CORRECTION:
		std::cout << "unsupported correction trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_DIRECT_BRANCH_WS:
//		cout << "| direct branch with sync\n";
		status = parseDirectBranchWS(nm,analytics);
		break;
	case TraceDqr::TCODE_INDIRECT_BRANCH_WS:
//		cout << "| indirect branch with sync";
		status = parseIndirectBranchWS(nm,analytics);
		break;
	case TraceDqr::TCODE_DATA_WRITE_WS:
		std::cout << "unsupported data write with sync trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_DATA_READ_WS:
		std::cout << "unsupported data read with sync trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_WATCHPOINT:
		std::cout << "unsupported watchpoint trace message\n";
		status = TraceDqr::DQERR_ERR;
		break;
	case TraceDqr::TCODE_CORRELATION:
		status = parseCorrelation(nm,analytics);
		break;
	case TraceDqr::TCODE_AUXACCESS_WRITE:
		status = parseAuxAccessWrite(nm,analytics);
		break;
	default:
		std::cout << "Error: readNextTraceMsg(): Unknown TCODE " << std::hex << tcode << std::dec << std::endl;
		status = TraceDqr::DQERR_ERR;
	}

	if ((status != TraceDqr::DQERR_OK) && (firstMsg == true)) {
		std::cout << "Error possibly due to corrupted first message in trace - skipping message" << std::endl;

		firstMsg = false;

		return readNextTraceMsg(nm,analytics);
	}

	firstMsg = false;

	return status;
}

#ifdef foo
linkedNexusMessage *linkedNexusMessage::firstMsg = nullptr;
int                 linkedNexusMessage::lastCore = -1;
linkedNexusMessage *linkedNexusMessage::linkedNexusMessageHeads[8];
linkedNexusMessage *linkedNexusMessage::lastNexusMsgPtr[8];

linkedNexusMessage::linkedNexusMessage()
{
	nextCoreMessage = nullptr;
	nextInOrderMessage = nullptr;
	consumed = false;
}

void linkedNexusMessage::init()
{
	firstMsg = nullptr;
	lastCore = -1;

    for (int i = 0; i < (int)(sizeof linkedNexusMessageHeads) / (int)(sizeof linkedNexusMessageHeads[0]); i++) {
    	linkedNexusMessageHeads[i] = nullptr;
    	lastNexusMsgPtr[i] = nullptr;
    }
}

dqr::DQErr linkedNexusMessage::buildLinkedMsgs(NexusMessage &nm)
{
	linkedNexusMessage *lnm = new linkedNexusMessage;

	int core = nm.src;

	if ((core < 0) || (core >= (int)(sizeof linkedNexusMessageHeads) / (int)(sizeof linkedNexusMessageHeads[0]))) {
		printf("Error: SliceFileParser::buildLinkedMsgs(): coreID out of bounds: %d\n",core);

		return dqr::DQERR_ERR;
	}

	lnm->nm = nm;

	if (firstMsg == nullptr) {
		firstMsg = lnm;
	}

	if (linkedNexusMessageHeads[core] == nullptr) {
		linkedNexusMessageHeads[core] = lnm;

		if (lastCore != -1) {
			lastNexusMsgPtr[lastCore]->nextInOrderMessage = lnm;
		}
	}
	else {
		lastNexusMsgPtr[core]->nextCoreMessage = lnm;
		lastNexusMsgPtr[lastCore]->nextInOrderMessage = lnm;
	}

	lastNexusMsgPtr[core] = lnm;
	lastCore = core;

	return dqr::DQERR_OK;
}

dqr::DQErr SliceFileParser::readAllTraceMsgs()	// generator to return trace messages one at a time
{
    NexusMessage nm;
    dqr::DQErr rc;

    linkedNexusMessage::init();

	while ((rc = readNextTraceMsg(nm)) == dqr::DQERR_OK) {
		// stick message in the Q with linkage!

		rc = linkedNexusMessage::buildLinkedMsgs(nm);
		if (rc != dqr::DQERR_OK) {
			return rc;
		}
	}

	return dqr::DQERR_OK;
}
#endif // foo

Disassembler::Disassembler(bfd *abfd)
{
	assert(abfd != nullptr);

	this->abfd = abfd;

    prev_index       = -1;
    cached_sym_index = -1;
    cached_sym_vma   = 0;
    cached_sym_size  = 0;

    start_address = bfd_get_start_address(abfd);

    long storage_needed;

    storage_needed = bfd_get_symtab_upper_bound(abfd);

//    printf("storage needed = %d\n",storage_needed);

    if (storage_needed > 0) {
    	symbol_table = (asymbol **)new (std::nothrow) char[storage_needed];

    	assert(symbol_table != nullptr);

    	number_of_syms = bfd_canonicalize_symtab(abfd,symbol_table);

//    	printf("symbol table @ %08x, num syms: %d\n",symbol_table,number_of_syms);

    	if (number_of_syms > 0) {
    		sorted_syms = new (std::nothrow) asymbol*[number_of_syms];

    		assert(sorted_syms != nullptr);

//    		printf("sorted syms @ %08x\n",sorted_syms);

    		for (int i = 0; i < number_of_syms; i++) {
    			sorted_syms[i] = symbol_table[i];
    		}

    		// could probably use C++ template sort here, but this works for now

//    		for (int i = 0; i < number_of_syms; i++) {
//    			printf("symbol[%d]:%s, addr:%08x, flags:%08x\n",i,sorted_syms[i]->name,bfd_asymbol_value(sorted_syms[i]),sorted_syms[i]->flags);
//    		}

    		// note: qsort does not preserver order on equal items!

    		qsort((void*)sorted_syms,(size_t)number_of_syms,sizeof sorted_syms[0],asymbol_compare_func);

    		func_info = new (std::nothrow) func_info_t[number_of_syms];

    		assert(func_info != nullptr);

    		// compute sizes of functions and data items for lookup by address

    		for (int i = 0; i < number_of_syms; i++) {
				func_info[i].sym_flags = sorted_syms[i]->flags;

				if ((sorted_syms[i]->flags & (BSF_FUNCTION | BSF_OBJECT)) != 0) {

    				// set func_vma to the base+offset address, not just the offest

    				func_info[i].func_vma = bfd_asymbol_value(sorted_syms[i]);

    				int j;

    				for (j = i+1; j < number_of_syms; j++) {
    					if ((sorted_syms[j]->flags & (BSF_FUNCTION | BSF_OBJECT)) != 0) {
    						func_info[i].func_size = bfd_asymbol_value(sorted_syms[j]) - func_info[i].func_vma;
    						break;
    					}
    				}

    				if (j >= number_of_syms) {
    					// size size of func(i) to end of section

    					asection *section;
    					bfd_vma base_vma;
    					section = sorted_syms[i]->section;
    					base_vma = section->vma;

    					func_info[i].func_size = (base_vma + section->size) - func_info[i].func_vma;
    				}
    			}
    			else {
    				func_info[i].func_size = 0;	// this probably isn't right
    			}

//				printf("symbol[%d]:%s, addr:%08x size:%d, flags:%08x\n",i,sorted_syms[i]->name,func_info[i].func_vma,func_info[i].func_size,sorted_syms[i]->flags);
    		}
    	}
    }
    else {
    	symbol_table = nullptr;
    	sorted_syms = nullptr;
    }

    codeSectionLst = nullptr;

    for (asection *p = abfd->sections; p != NULL; p = p->next) {
  	  if (p->flags & SEC_CODE) {
            // found a code section, add to list

  		  section *sp = new section;
  		  if (sp->initSection(&codeSectionLst, p) == nullptr) {
  			  status = TraceDqr::DQERR_ERR;
  			  return;
  		  }
  	  }
    }

    if (codeSectionLst == nullptr) {
    	printf("Error: no code sections found\n");

    	status = TraceDqr::DQERR_ERR;
    	return;
    }

   	info = new (std::nothrow) disassemble_info;

   	assert(info != nullptr);

   	disassemble_func = disassembler(bfd_get_arch(abfd), bfd_big_endian(abfd),bfd_get_mach(abfd), abfd);
   	if (disassemble_func == nullptr) {
   		printf("Error: Disassmbler::Disassembler(): could not create disassembler\n");

   		delete [] info;
   		info = nullptr;

   		status = TraceDqr::DQERR_ERR;
   		return;
   	}

   	init_disassemble_info(info,stdout,(fprintf_ftype)stringify_callback);

   	info->print_address_func = override_print_address;
   	info->arch = bfd_get_arch(abfd);
   	info->mach = bfd_get_mach(abfd);

   	info->buffer_vma = codeSectionLst->startAddr;
   	info->buffer_length = codeSectionLst->size;
   	info->section = codeSectionLst->asecptr;
   	info->buffer = (bfd_byte*)codeSectionLst->code;

//   	info->buffer_vma = text->vma;
//   	info->buffer_length = text->size;
//   	info->section = text;
//   	bfd_malloc_and_get_section(abfd,text,&info->buffer);

   	info->application_data = (void*)this;

   	disassemble_init_for_target(info);

   	fileReader = new class fileReader();

   	lastFileName = nullptr;
   	lastLineNumber = -1;

    status = TraceDqr::DQERR_OK;
}

int Disassembler::lookupInstructionByAddress(bfd_vma vma,uint32_t *ins,int *ins_size)
{
	assert(ins != nullptr);

	uint32_t inst;
	int size;
	int rc;

	// need to support multiple code sections and do some error checking on the address!

	if (codeSectionLst == nullptr) {
		status = TraceDqr::DQERR_ERR;

		return 1;
	}

	section *sp;

	sp = codeSectionLst->getSectionByAddress((TraceDqr::ADDRESS)vma);

	if (sp == nullptr) {
		status = TraceDqr::DQERR_ERR;

		return 1;
	}

	inst = (uint32_t)sp->code[(vma - sp->startAddr)/2];

	rc = decodeInstructionSize(inst, size);
	if (rc != 0) {
		status = TraceDqr::DQERR_ERR;

		return rc;
	}

	*ins_size = size;

	if (size == 16) {
//    printf("instruction: %04x\n",inst);
		*ins = inst;
	}
	else {
		*ins = (((uint32_t)sp->code[(vma - sp->startAddr)/2+1]) << 16) | inst;

//    printf("instruction: %08x\n",*ins);
	}

	return 0;
}

int Disassembler::lookup_symbol_by_address(bfd_vma vma,flagword flags,int *index,int *offset)
{
	assert(index != nullptr);
	assert(offset != nullptr);

	// find the function closest to the address. Address should either be start of function, or in body
	// of function.

	if ( vma == 0) {
		return 0;
	}

	//asymbol **syms;
	int i;

	// check for a cache hit

	if (cached_sym_index != -1) {
		if ((vma >= cached_sym_vma) && (vma < (cached_sym_vma + cached_sym_size))) {
			*index = cached_sym_index;
			*offset = vma - cached_sym_vma;

			return 1;
		}
	}

	//syms = sorted_syms;

	for (i = 0; i < number_of_syms; i++) {
		if ((func_info[i].sym_flags & (BSF_FUNCTION | BSF_OBJECT)) != 0) {

			// note: func_vma already has the base+offset address in it

			if (vma == func_info[i].func_vma) {
				// exact match on vma. Make sure function isn't zero sized
				// if it is, try to find a following one at the same address

//				printf("\n->vma:%08x size: %d, i=%d, name: %s\n",vma,func_info[i].func_size,i,sorted_syms[i]->name);

				for (int j = i+1; j < number_of_syms; j++) {
					if ((func_info[j].sym_flags & (BSF_FUNCTION | BSF_OBJECT)) != 0) {
						if (func_info[i].func_vma == func_info[j].func_vma) {
							i = j;
						}
					}
				}

				// have a match with a function

				//printf("have match. Index %d\n",i);

				// cache it for re-lookup speed improvement

				cached_sym_index = i;
				cached_sym_vma = func_info[i].func_vma;
				cached_sym_size = func_info[i].func_size;

				*index = i;
				*offset = vma - cached_sym_vma;

				return 1;
			}
			else if (vma > func_info[i].func_vma) {
				if (vma < (func_info[i].func_vma + func_info[i].func_size)) {

					cached_sym_index = i;
					cached_sym_vma = func_info[i].func_vma;
					cached_sym_size = func_info[i].func_size;

					*index = i;
					*offset = vma - cached_sym_vma;

					return 1;
				}
			}
		}
	}

	return 0;
}

void Disassembler::overridePrintAddress(bfd_vma addr, struct disassemble_info *info)
{
	assert(info != nullptr);

	//	lookup symbol at addr.

	// use field in info to point to disassembler object so we can call member funcs

	if (info != this->info) {
		printf("Error: overridePrintAddress(): info does not match\n");
	}

	instruction.operandAddress = addr;
	instruction.haveOperandAddress = true;

	int index;
	int offset;
	int rc;

	rc = lookup_symbol_by_address(addr,BSF_FUNCTION,&index,&offset);

	if (rc != 0) {

		// found symbol

		instruction.operandLabel = sorted_syms[index]->name;
		instruction.operandLabelOffset = offset;

//		if (offset == 0) {
//			printf("%08lx <%s>",addr,sorted_syms[index]->name);
//		}
//		else {
//			printf("%08lx <%s+%x>",addr,sorted_syms[index]->name,offset);
//		}
	}
	else {
//		printf("%08lx",addr);

		instruction.operandLabel = nullptr;
		instruction.operandLabelOffset = 0;
	}
}


void Disassembler::print_address(bfd_vma vma)
{
	// find closest preceeding function symbol and print with offset

	int index;
	int offset;
	int rc;

	rc = lookup_symbol_by_address(vma,BSF_FUNCTION,&index,&offset);
	if (rc != 0) {
		// found symbol

	    if (prev_index != index) {
	    	prev_index = index;

//	    	printf("\n");

	    	if (offset == 0) {
	    		printf("%08lx <%s>:\n",vma,sorted_syms[index]->name);
	    	}
	    	else {
	    		printf("%08lx <%s+%x>:\n",vma,sorted_syms[index]->name,offset);
	    	}
	    }

	    printf("%8lx: ",vma);
	}
	else {
		// didn't find symbol

		prev_index = -1;

		printf("%8lx:",vma);
	}
}

void Disassembler::print_address_and_instruction(bfd_vma vma)
{
	uint32_t ins = 0;
	int ins_size = 0;

	print_address(vma);

	lookupInstructionByAddress(vma,&ins,&ins_size);

	if (ins_size > 16) {
		 printf("       %08x                ",ins);
	}
	else {
		 printf("       %04x                    ",ins);
	}
}

void Disassembler::setInstructionAddress(bfd_vma vma)
{
	instruction.address = vma;

	// find closest preceeding function symbol and print with offset

	int index;
	int offset;
	int rc;

	lookupInstructionByAddress(vma,&instruction.instruction,&instruction.instSize);

	rc = lookup_symbol_by_address(vma,BSF_FUNCTION,&index,&offset);
	if (rc == 0) {
		// did not find symbol

		instruction.addressLabel = nullptr;
		instruction.addressLabelOffset = 0;
	}
	else {

		// found symbol

		instruction.addressLabel = sorted_syms[index]->name;
		instruction.addressLabelOffset = offset;
	}
}

int Disassembler::decodeInstructionSize(uint32_t inst, int &inst_size)
{
	switch (inst & 0x0003) {
	case 0x0000:	// quadrant 0, compressed
		inst_size = 16;
		return 0;
	case 0x0001:	// quadrant 1, compressed
		inst_size = 16;
		return 0;
	case 0x0002:	// quadrant 2, compressed
		inst_size = 16;
		return 0;
	case 0x0003:	// not compressed. Assume RV32 for now
		if ((inst & 0x1f) == 0x1f) {
			fprintf(stderr,"Error: decode_instruction(): cann't decode instructions longer than 32 bits\n");
			return 1;
		}

		inst_size = 32;
		return 0;
	}

	// error return

	return 1;
}

#define MOVE_BIT(bits,s,d)	((bits&(1<<s))?(1<<d):0)

int Disassembler::decodeRV32Q0Instruction(uint32_t instruction,int &inst_size,TraceDqr::InstType &inst_type,int32_t &immeadiate,bool &is_branch)
{
	// no branch or jump instruction in quadrant 0

	inst_size = 16;
	is_branch = false;
	immeadiate = 0;

	if ((instruction & 0x0003) != 0x0000) {
		return 1;
	}

	inst_type = TraceDqr::INST_UNKNOWN;

	return 0;
}

int Disassembler::decodeRV32Q1Instruction(uint32_t instruction,int &inst_size,TraceDqr::InstType &inst_type,int32_t &immeadiate,bool &is_branch)
{
	// Q1 compressed instruction

	uint32_t t;

	inst_size = 16;

	switch (instruction >> 13) {
	case 0x1:
		inst_type = TraceDqr::INST_C_JAL;
		is_branch = true;

		t = MOVE_BIT(instruction,3,1);
		t |= MOVE_BIT(instruction,4,2);
		t |= MOVE_BIT(instruction,5,3);
		t |= MOVE_BIT(instruction,11,4);
		t |= MOVE_BIT(instruction,2,5);
		t |= MOVE_BIT(instruction,7,6);
		t |= MOVE_BIT(instruction,6,7);
		t |= MOVE_BIT(instruction,9,8);
		t |= MOVE_BIT(instruction,10,9);
		t |= MOVE_BIT(instruction,8,10);
		t |= MOVE_BIT(instruction,12,11);

		if (t & (1<<11)) { // sign extend offset
			t |= 0xfffff000;
		}

		immeadiate = t;
		break;
	case 0x5:
		inst_type = TraceDqr::INST_C_J;
		is_branch = true;

		t = MOVE_BIT(instruction,3,1);
		t |= MOVE_BIT(instruction,4,2);
		t |= MOVE_BIT(instruction,5,3);
		t |= MOVE_BIT(instruction,11,4);
		t |= MOVE_BIT(instruction,2,5);
		t |= MOVE_BIT(instruction,7,6);
		t |= MOVE_BIT(instruction,6,7);
		t |= MOVE_BIT(instruction,9,8);
		t |= MOVE_BIT(instruction,10,9);
		t |= MOVE_BIT(instruction,8,10);
		t |= MOVE_BIT(instruction,12,11);

		if (t & (1<<11)) { // sign extend offset
			t |= 0xfffff000;
		}

		immeadiate = t;
		break;
	case 0x6:
		inst_type = TraceDqr::INST_C_BEQZ;
		is_branch = true;

		t = MOVE_BIT(instruction,3,1);
		t |= MOVE_BIT(instruction,4,2);
		t |= MOVE_BIT(instruction,10,3);
		t |= MOVE_BIT(instruction,11,4);
		t |= MOVE_BIT(instruction,2,5);
		t |= MOVE_BIT(instruction,5,6);
		t |= MOVE_BIT(instruction,6,7);
		t |= MOVE_BIT(instruction,12,8);

		if (t & (1<<8)) { // sign extend offset
			t |= 0xfffffe00;
		}

		immeadiate = t;
		break;
	case 0x7:
		inst_type = TraceDqr::INST_C_BNEZ;
		is_branch = true;

		t = MOVE_BIT(instruction,3,1);
		t |= MOVE_BIT(instruction,4,2);
		t |= MOVE_BIT(instruction,10,3);
		t |= MOVE_BIT(instruction,11,4);
		t |= MOVE_BIT(instruction,2,5);
		t |= MOVE_BIT(instruction,5,6);
		t |= MOVE_BIT(instruction,6,7);
		t |= MOVE_BIT(instruction,12,8);

		if (t & (1<<8)) { // sign extend offset
			t |= 0xfffffe00;
		}

		immeadiate = t;
		break;
	default:
		inst_type = TraceDqr::INST_UNKNOWN;
		is_branch = 0;
		break;
	}

	return 0;
}

int Disassembler::decodeRV32Q2Instruction(uint32_t instruction,int &inst_size,TraceDqr::InstType &inst_type,int32_t &immeadiate,bool &is_branch)
{
	// Q1 compressed instruction

	inst_size = 16;

	inst_type = TraceDqr::INST_UNKNOWN;
	is_branch = false;

	switch (instruction >> 13) {
	case 0x4:
		if (instruction & (1<<12)) {
			if ((instruction & 0x007c) == 0x0000) {
				if ((instruction & 0x0f80) != 0x0000) {
					inst_type = TraceDqr::INST_C_JALR;
					is_branch = true;
					immeadiate = 0;
				}
			}
		}
		else {
			if ((instruction & 0x007c) == 0x0000) {
				if ((instruction & 0x0f80) != 0x0000) {
					inst_type = TraceDqr::INST_C_JR;
					is_branch = true;
					immeadiate = 0;
				}
			}
		}
		break;
	default:
		break;
	}

	return 0;
}

int Disassembler::decodeRV32Instruction(uint32_t instruction,int &inst_size,TraceDqr::InstType &inst_type,int32_t &immeadiate,bool &is_branch)
{
	if ((instruction & 0x1f) == 0x1f) {
		fprintf(stderr,"Error: decodeBranch(): cann't decode instructions longer than 32 bits\n");
		return 1;
	}

	uint32_t t;

	inst_size = 32;

	switch (instruction & 0x7f) {
	case 0x6f:
		inst_type = TraceDqr::INST_JAL;
		is_branch = true;

		t = MOVE_BIT(instruction,21,1);
		t |= MOVE_BIT(instruction,22,2);
		t |= MOVE_BIT(instruction,23,3);
		t |= MOVE_BIT(instruction,24,4);
		t |= MOVE_BIT(instruction,25,5);
		t |= MOVE_BIT(instruction,26,6);
		t |= MOVE_BIT(instruction,27,7);
		t |= MOVE_BIT(instruction,28,8);
		t |= MOVE_BIT(instruction,29,9);
		t |= MOVE_BIT(instruction,30,10);
		t |= MOVE_BIT(instruction,20,11);
		t |= MOVE_BIT(instruction,12,12);
		t |= MOVE_BIT(instruction,13,13);
		t |= MOVE_BIT(instruction,14,14);
		t |= MOVE_BIT(instruction,15,15);
		t |= MOVE_BIT(instruction,16,16);
		t |= MOVE_BIT(instruction,17,17);
		t |= MOVE_BIT(instruction,18,18);
		t |= MOVE_BIT(instruction,19,19);
		t |= MOVE_BIT(instruction,31,20);

		if (t & (1<<20)) { // sign extend offset
			t |= 0xffe00000;
		}

		immeadiate = t;
		break;
	case 0x67:
		if ((instruction & 0x7000) == 0x000) {
			inst_type = TraceDqr::INST_JALR;
			is_branch = true;

			t = instruction >> 20;

			if (t & (1<<11)) { // sign extend offset
				t |= 0xfffff000;
			}

			immeadiate = t;
		}
		else {
			inst_type = TraceDqr::INST_UNKNOWN;
			is_branch = false;
		}
		break;
	case 0x63:
		switch ((instruction >> 12) & 0x7) {
		case 0x0:
			inst_type = TraceDqr::INST_BEQ;
			break;
		case 0x1:
			inst_type = TraceDqr::INST_BNE;
			break;
		case 0x4:
			inst_type = TraceDqr::INST_BLT;
			break;
		case 0x5:
			inst_type = TraceDqr::INST_BGE;
			break;
		case 0x6:
			inst_type = TraceDqr::INST_BLTU;
			break;
		case 0x7:
			inst_type = TraceDqr::INST_BGEU;
			break;
		default:
			inst_type = TraceDqr::INST_UNKNOWN;
			is_branch = false;
			return 0;
		}

		is_branch = true;

		t = MOVE_BIT(instruction,8,1);
		t |= MOVE_BIT(instruction,9,2);
		t |= MOVE_BIT(instruction,10,3);
		t |= MOVE_BIT(instruction,11,4);
		t |= MOVE_BIT(instruction,25,5);
		t |= MOVE_BIT(instruction,26,6);
		t |= MOVE_BIT(instruction,27,7);
		t |= MOVE_BIT(instruction,28,8);
		t |= MOVE_BIT(instruction,29,9);
		t |= MOVE_BIT(instruction,30,10);
		t |= MOVE_BIT(instruction,7,11);
		t |= MOVE_BIT(instruction,31,12);

		if (t & (1<<12)) { // sign extend offset
			t |= 0xffffe000;
		}

		immeadiate = t;
		break;
	}

	return 0;
}

int Disassembler::decodeInstruction(uint32_t instruction,int &inst_size,TraceDqr::InstType &inst_type,int32_t &immeadiate,bool &is_branch)
{
	int rc;

	switch (instruction & 0x0003) {
	case 0x0000:	// quadrant 0, compressed
		rc = decodeRV32Q0Instruction(instruction,inst_size,inst_type,immeadiate,is_branch);
		break;
	case 0x0001:	// quadrant 1, compressed
		rc = decodeRV32Q1Instruction(instruction,inst_size,inst_type,immeadiate,is_branch);
		break;
	case 0x0002:	// quadrant 2, compressed
		rc = decodeRV32Q2Instruction(instruction,inst_size,inst_type,immeadiate,is_branch);
		break;
	case 0x0003:	// not compressed. Assume RV32 for now
		rc = decodeRV32Instruction(instruction,inst_size,inst_type,immeadiate,is_branch);
		break;
	}

	return rc;
}

int Disassembler::getSrcLines(TraceDqr::ADDRESS addr, const char **filename, const char **functionname, unsigned int *linenumber, const char **lineptr)
{
	const char *file = nullptr;
	const char *function = nullptr;
	unsigned int line = 0;
	unsigned int discrim;

	// need to loop through all sections with code below and try to find one that succeeds

	section *sp;

	*filename = nullptr;
	*functionname = nullptr;
	*linenumber = 0;
	*lineptr = nullptr;

	if (codeSectionLst == nullptr) {
		return 0;
	}

	sp = codeSectionLst->getSectionByAddress(addr);

	if (sp == nullptr) {
		return 0;
	}

	if (bfd_find_nearest_line_discriminator(abfd,sp->asecptr,symbol_table,addr-sp->startAddr,&file,&function,&line,&discrim) == 0) {
		return 0;
	}

	*linenumber = line;

	if (file == nullptr) {
		return 0;
	}

	struct fileReader::fileList *fl;

	fl = fileReader->findFile(file);
	assert(fl != nullptr);

	*filename = fl->name;

	if (function != nullptr) {
		// find the function name in fncList

		struct fileReader::funcList *funcLst;

		for (funcLst = fl->funcs; (funcLst != nullptr) && (strcmp(funcLst->func,function) != 0); funcLst = funcLst->next) {
			// empty
		}

		if (funcLst == nullptr) {
			// add function to the list

			int len = strlen(function)+1;
			char *fname = new char[len];
			strcpy(fname,function);

			funcLst = new fileReader::funcList;
			funcLst->func = fname;
			funcLst->next = fl->funcs;
			fl->funcs = funcLst;
		}

		// at this point fl->funcs should point to this function (if it was just added or not)

		*functionname = fl->funcs->func;
	}

	// line numbers start at 1

	if ((line >= 1) && (line <= fl->lineCount)) {
		*lineptr = fl->lines[line-1];
	}

	return 1;
}

int Disassembler::Disassemble(TraceDqr::ADDRESS addr)
{
	assert(disassemble_func != nullptr);

	bfd_vma vma;
	vma = (bfd_vma)addr;

	getSrcLines(addr, &source.sourceFile, &source.sourceFunction, &source.sourceLineNum, &source.sourceLine);

	setInstructionAddress(vma);

	int rc;

	instruction.instructionText[0] = 0;
	dis_output = instruction.instructionText;

	instruction.haveOperandAddress = false;

	// before calling disassemble_func, need to update info struct to point to correct section!

	if (codeSectionLst == nullptr) {
		return 1;
	}

	section *sp = codeSectionLst->getSectionByAddress(addr);

	if (sp == nullptr) {
		return 1;
	}

   	info->buffer_vma = sp->startAddr;
   	info->buffer_length = sp->size;
   	info->section = sp->asecptr;

   	// potential memory leak below the first time this is done because buffer was initially allocated by bfd

   	info->buffer = (bfd_byte*)sp->code;

	rc = disassemble_func(vma,info);

	// output from disassemble_func is in instruction.instrucitonText

	return rc;
}

NexusMessageSync::NexusMessageSync()
{
	firstMsgNum = 0;
	lastMsgNum  = 0;
	index = 0;
}

