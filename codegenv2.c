
#include "codegenv2.h"

#include <time.h>
#include "globals.h"
#include "parser.h"
#include "segment.h"
#include "extern.h"
#include "fixup.h"
#include "fastpass.h"
#include "myassert.h"
#include "types.h"
#include "macro.h"
#include "listing.h"

#define OutputCodeByte( x ) OutputByte( x )

const char        szNullStr[] = { "<NULL>" };
struct Mem_Def*   MemTable    = NULL;
struct Instr_Def* InstrHash[16384];

#include "MemTable32.h"
#include "MemTable64.h"
#include "InstrTableV2.h"

static unsigned int hash(const uint_8* data, int size)
/******************************************/
{
	uint_64 fnv_basis = 14695981039346656037;
	uint_64 register fnv_prime = 1099511628211;
	uint_64 h = fnv_basis;
	int cnt = 0;
	for (cnt = 0; cnt<size; cnt++) {
		h ^= *data++;
		h *= fnv_prime;
	}
	return((((h >> 49) ^ h) & 0x3fff));
}

struct Instr_Def* AllocInstruction() 
{
	return malloc(sizeof(struct Instr_Def));
}

void InsertInstruction(struct Instr_Def* pInstruction, uint_32 hash)
{
	struct Instr_Def* curPtr = NULL;
	curPtr = InstrHash[hash];
	if (curPtr == NULL)
	{
		InstrHash[hash] = pInstruction;
		return;
	}
	while (curPtr->next != NULL)
	{
		curPtr = curPtr->next;
	}
	curPtr->next = pInstruction;
}

uint_32 GenerateInstrHash(struct Instr_Def* pInstruction)
{
	uint_8 hashBuffer[32];
	int len = 0;
	char* pDst = (char*)&hashBuffer;
	strcpy(pDst, pInstruction->mnemonic);
	len += strlen(pInstruction->mnemonic);
	pDst += len;
	*(pDst + 0) = pInstruction->operand_types[0];
	*(pDst + 1) = pInstruction->operand_types[1];
	*(pDst + 2) = pInstruction->operand_types[2];
	*(pDst + 3) = pInstruction->operand_types[3];
	*(pDst + 4) = pInstruction->operand_types[4];
	len  += 4;
	pDst += 4;
	return hash(&hashBuffer, len);
}

void BuildInstructionTable(void) 
{
	uint_32 hash = 0;
	struct Instr_Def* pInstrTbl = &InstrTableV2;
	memset(InstrHash, 0, sizeof(InstrHash));
	uint_32 i = 0;
	uint_32 instrCount = sizeof(InstrTableV2) / sizeof(struct Instr_Def);

	for (i = 0; i < instrCount; i++, pInstrTbl++)
	{
		struct Instr_Def* pInstr = AllocInstruction();
		memcpy(pInstr, pInstrTbl, sizeof(struct Instr_Def));
		hash = GenerateInstrHash(pInstr);
		InsertInstruction(pInstr, hash);
	}
}

/* =====================================================================
	Some instruction forms require specific registers such as AX or CL.
	Demotion allows us to check the instruction table twice, once using the explicit register should it exist,
	secondly after demotion to look for a generic case.
===================================================================== */
enum op_type DemoteOperand(enum op_type op) {
	enum op_type ret = op;

	if (op == R8_AL)
		ret = R8;
	else if (op == R16_AX)
		ret = R16;
	else if (op == R32_EAX)
		ret = R32;
	else if (op == R64_RAX)
		ret = R64;
	else if (op == R8_CL)
		ret = R8;
	else if (op == R16_CX)
		ret = R16;
	else if (op == R32_ECX)
		ret = R32;
	else if (op == R64_RCX)
		ret = R64;

	/* We must be careful that an instruction can only have one demotable operand at a time */
	else if (op == M8 || op == M16 || op == M32 || op == M48 || op == M64 || op == M80)
		ret = M_ANY;

	return(ret);
}

enum op_type MatchOperand(struct code_info *CodeInfo, struct opnd_item op, struct expr opExpr) {
	enum op_type result;
	switch (op.type)
	{
		case OP_M:
			result = M_ANY;
			break;
		case OP_M08:
			result = M8;
			break;
		case OP_M16:
			result = M16;
			break;
		case OP_M32:
			result = M32;
			break;
		case OP_M64:
			result = M64;
			break;
		case OP_M48:
			result = M48;
			break;
		case OP_M80:
			result = M80;
			break;
		case OP_M128:
			result = M128;
			break;
		case OP_M256:
			result = M256;
			break;
		case OP_M512:
			result = M512;
			break;
		case OP_SR86:
			result = R_SEG;
			break;
		case OP_SR:
			result = R_SEG;
			break;
		case OP_SR386:
			result = R_SEGE;
			break;
		case OP_RSPEC:
			result = R_RIP;
			/* If the register operand is cr0-cr8 (Parser error generates OP.type == RSPEC for these) */
			if (strcasecmp(opExpr.base_reg->string_ptr, "cr0") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "cr2") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "cr3") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "cr4") == 0)
			{
				result = R_CR;
			}
			else if (strcasecmp(opExpr.base_reg->string_ptr, "cr8") == 0)
			{
				result = R_CR8;
			}
			/* If the register operand is dr0-dr7 (Parser error generates OP.type == R_RIP for these) */
			else if (strcasecmp(opExpr.base_reg->string_ptr, "dr0") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr1") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr2") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr3") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr4") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr5") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr6") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr7") == 0)
			{
				result = R_DR;
			}
			break;
		case OP_K:
			result = R_K;
			break;
		case OP_RIP:
			result = R_RIP;
			/* If the register operand is cr0-cr8 (Parser error generates OP.type == R_RIP for these) */
			if (strcasecmp(opExpr.base_reg->string_ptr, "cr0") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr2") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr3") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr4") == 0 ||
			strcasecmp(opExpr.base_reg->string_ptr, "cr8") == 0)
			{
				result = R_CR;
			}
			else if (strcasecmp(opExpr.base_reg->string_ptr, "cr8") == 0)
			{
				result = R_CR8;
			}
			/* If the register operand is dr0-dr7 (Parser error generates OP.type == R_RIP for these) */
			else if (strcasecmp(opExpr.base_reg->string_ptr, "dr0") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr1") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr2") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr3") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr4") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dr5") == 0 || 
				strcasecmp(opExpr.base_reg->string_ptr, "dr6") == 0 || 
				strcasecmp(opExpr.base_reg->string_ptr, "dr7") == 0)
			{
				result = R_DR;
			}
			break;
		case OP_AL:
			result = R8_AL;
			break;
		case OP_CL:
			result = R8_CL;
			break;
		case OP_AX:
			result = R16_AX;
			break;
		case OP_EAX:
			result = R32_EAX;
			break;
		case OP_RAX:
			result = R64_RAX;
			break;
		case OP_NONE:
			result = OP_N;
			break;
		case OP_R8:
			result = R8;
			
			/* If AL is somehow passed in as an OP_R8, promote it first */
			if (strcasecmp(opExpr.base_reg->string_ptr, "al") == 0)
				result = R8_AL;

			/* If the register operand is ah,bh,ch,dh */
			if (strcasecmp(opExpr.base_reg->string_ptr, "ah") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "bh") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "ch") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dh") == 0)
			{
				result = R8H;
			}
			/* If the register operand is r8b-r15b */
			else if (strcasecmp(opExpr.base_reg->string_ptr, "r8b") == 0 ||
					 strcasecmp(opExpr.base_reg->string_ptr, "r9b") == 0 ||
					 strcasecmp(opExpr.base_reg->string_ptr, "r10b") == 0 ||
					 strcasecmp(opExpr.base_reg->string_ptr, "r11b") == 0 ||
					 strcasecmp(opExpr.base_reg->string_ptr, "r12b") == 0 ||
					 strcasecmp(opExpr.base_reg->string_ptr, "r13b") == 0 ||
					 strcasecmp(opExpr.base_reg->string_ptr, "r14b") == 0 ||
					 strcasecmp(opExpr.base_reg->string_ptr, "r15b") == 0)
			{
				result = R8E;
			}
			else if (strcasecmp(opExpr.base_reg->string_ptr, "sil") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "dil") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "spl") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "bpl") == 0)
			{
				result = R8U;
			}
			break;
		case OP_DX:
			result = R16;
			break;
		case OP_R16:
			result = R16;

			/* If AX is somehow passed in as an OP_R16, promote it first */
			if (strcasecmp(opExpr.base_reg->string_ptr, "ax") == 0)
				result = R16_AX;

			/* If the register operand is r8w-r15w */
			if (strcasecmp(opExpr.base_reg->string_ptr, "r8w") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r9w") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r10w") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r11w") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r12w") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r13w") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r14w") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r15w") == 0)
			{
				result = R16E;
			}
			break;
		case OP_R32:
			result = R32;

			/* If EAX is somehow passed in as an OP_R32, promote it first */
			if (strcasecmp(opExpr.base_reg->string_ptr, "eax") == 0)
				result = R32_EAX;

			/* If the register operand is r8d-r15d */
			if (strcasecmp(opExpr.base_reg->string_ptr, "r8d") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r9d") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r10d") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r11d") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r12d") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r13d") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r14d") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r15d") == 0)
			{
				result = R32E;
			}
			break;
		case OP_R64:
			result = R64;
			
			/* If RAX is somehow passed in as an OP_R64, promote it first */
			if (strcasecmp(opExpr.base_reg->string_ptr, "rax") == 0)
				result = R64_RAX;

			/* If the register operand is r8-r15 */
			if (strcasecmp(opExpr.base_reg->string_ptr, "r8") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r9") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r10") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r11") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r12") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r13") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r14") == 0 ||
				strcasecmp(opExpr.base_reg->string_ptr, "r15") == 0)
			{
				result = R64E;
			}
			break;
		case OP_I8:
			result = IMM8;
			break;
		case OP_I16:
			result = IMM16;
			break;
		case OP_I32:
			result = IMM32;
			break;
		case OP_I64:
			result = IMM64;
			break;
		case OP_XMM:
			result = SSE128;
			/* If register xmm8-xmm15 USE AVX128 */
			/* If register xmm16-xmm31 USE AVX512_128 */
			break;
		case OP_YMM:
			result = AVX256;
			/* If register ymm16-ymm31 USE AVX512_256 */
			break;
		case OP_ZMM:
			result = AVX512;
			break;
	}
	return result;
}

struct Instr_Def* LookupInstruction(struct Instr_Def* instr, bool memReg) {
	uint_32           hash;
	struct Instr_Def* pInstruction = NULL;
	bool              matched      = FALSE;

	hash = GenerateInstrHash(instr);
    pInstruction = InstrHash[hash];
	while (pInstruction != NULL)
	{
		if (strcasecmp(pInstruction->mnemonic, instr->mnemonic) == 0 &&
			pInstruction->operand_types[0] == instr->operand_types[0] &&
			pInstruction->operand_types[1] == instr->operand_types[1] &&
			pInstruction->operand_types[2] == instr->operand_types[2] &&
			pInstruction->operand_types[3] == instr->operand_types[3]) {
			/* If the instruction only supports absolute or displacement only addressing 
			and we have a register in the memory expression, this is not a match */
			if (memReg && ((pInstruction->flags & NO_MEM_REG) != 0))
				goto nextInstr;
			matched = TRUE;
			break;
		}
		nextInstr:
		pInstruction = pInstruction->next;
	}
	if (!matched)
		pInstruction = NULL;
	return pInstruction;
}

bool Require_OPND_Size_Override(struct Instr_Def* instr, struct code_info* CodeInfo)
{
	if (instr->useOSO == OP_SIZE_OVERRIDE)
	{
		if (instr->op_size == 2 && (ModuleInfo.Ofssize == USE32 || ModuleInfo.Ofssize == USE64))
			return TRUE;
		if (instr->op_size == 4 && ModuleInfo.Ofssize == USE16)
			return TRUE;
	}
	return FALSE;
}

bool Require_ADDR_Size_Override(struct Instr_Def* instr, struct code_info* CodeInfo)
{
	return FALSE;
}

bool IsValidInCPUMode(struct Instr_Def* instr)
{
	bool          result   = TRUE;
	unsigned char cpuModes = instr->validModes;

	/* Don't allow a 64bit instruction in a non 64bit segment */
	if (ModuleInfo.Ofssize != USE64 && cpuModes == X64)
		result = FALSE;

	if (ModuleInfo.Ofssize == USE64 && (cpuModes & X64) == 0)
		result = FALSE;

	/* Are we allowing assembly of priviledge instructions? */
	if ((instr->cpu & P_PM) > (ModuleInfo.curr_cpu & P_PM))
		result = FALSE;

	return result;
}

/* =====================================================================
  Given an input token (string) for a register name, match it and return
  the correct register number for encoding reg/rm fields.
  ===================================================================== */
unsigned char GetRegisterNo(struct asm_tok *regTok)
{
	unsigned char regNo = 17;
	if (regTok)
	{
		switch (regTok->tokval)
		{
		/* 8bit */
		case T_AL:
			regNo = 0;
			break;
		case T_CL:
			regNo = 1;
			break;
		case T_DL:
			regNo = 2;
			break;
		case T_BL:
			regNo = 3;
			break;
		case T_AH:
			regNo = 4;
			break;
		case T_SPL:
			regNo = 4;
			break;
		case T_CH:
			regNo = 5;
			break;
		case T_BPL:
			regNo = 5;
			break;
		case T_DH:
			regNo = 6;
			break;
		case T_SIL:
			regNo = 6;
			break;
		case T_BH:
			regNo = 7;
			break;
		case T_DIL:
			regNo = 7;
			break;
		case T_R8B:
			regNo = 8;
			break;
		case T_R9B:
			regNo = 9;
			break;
		case T_R10B:
			regNo = 10;
			break;
		case T_R11B:
			regNo = 11;
			break;
		case T_R12B:
			regNo = 12;
			break;
		case T_R13B:
			regNo = 13;
			break;
		case T_R14B:
			regNo = 14;
			break;
		case T_R15B:
			regNo = 15;
			break;
		/* 16bit */
		case T_AX:
			regNo = 0;
			break;
		case T_CX:
			regNo = 1;
			break;
		case T_DX:
			regNo = 2;
			break;
		case T_BX:
			regNo = 3;
			break;
		case T_SP:
			regNo = 4;
			break;
		case T_BP:
			regNo = 5;
			break;
		case T_SI:
			regNo = 6;
			break;
		case T_DI:
			regNo = 7;
			break;
		case T_R8W:
			regNo = 8;
			break;
		case T_R9W:
			regNo = 9;
			break;
		case T_R10W:
			regNo = 10;
			break;
		case T_R11W:
			regNo = 11;
			break;
		case T_R12W:
			regNo = 12;
			break;
		case T_R13W:
			regNo = 13;
			break;
		case T_R14W:
			regNo = 14;
			break;
		case T_R15W:
			regNo = 15;
			break;
		/* 32bit */
		case T_EAX:
			regNo = 0;
			break;
		case T_ECX:
			regNo = 1;
			break;
		case T_EDX:
			regNo = 2;
			break;
		case T_EBX:
			regNo = 3;
			break;
		case T_ESP:
			regNo = 4;
			break;
		case T_EBP:
			regNo = 5;
			break;
		case T_ESI:
			regNo = 6;
			break;
		case T_EDI:
			regNo = 7;
			break;
		case T_R8D:
			regNo = 8;
			break;
		case T_R9D:
			regNo = 9;
			break;
		case T_R10D:
			regNo = 10;
			break;
		case T_R11D:
			regNo = 11;
			break;
		case T_R12D:
			regNo = 12;
			break;
		case T_R13D:
			regNo = 13;
			break;
		case T_R14D:
			regNo = 14;
			break;
		case T_R15D:
			regNo = 15;
			break;
		/* 64bit */
		case T_RAX:
			regNo = 0;
			break;
		case T_RCX:
			regNo = 1;
			break;
		case T_RDX:
			regNo = 2;
			break;
		case T_RBX:
			regNo = 3;
			break;
		case T_RSP:
			regNo = 4;
			break;
		case T_RBP:
			regNo = 5;
			break;
		case T_RSI:
			regNo = 6;
			break;
		case T_RDI:
			regNo = 7;
			break;
		case T_R8:
			regNo = 8;
			break;
		case T_R9:
			regNo = 9;
			break;
		case T_R10:
			regNo = 10;
			break;
		case T_R11:
			regNo = 11;
			break;
		case T_R12:
			regNo = 12;
			break;
		case T_R13:
			regNo = 13;
			break;
		case T_R14:
			regNo = 14;
			break;
		case T_R15:
			regNo = 15;
			break;
		/* specials */
		case T_RIP:
			regNo = 16;
			break;
		case T_CR0:
			regNo = 0;
			break;
		case T_CR2:
			regNo = 2;
			break;
		case T_CR3:
			regNo = 3;
			break;
		case T_CR4:
			regNo = 4;
			break;
		case T_CR8:
			regNo = 8;
			break;
		case T_DR0:
			regNo = 0;
			break;
		case T_DR1:
			regNo = 1;
			break;
		case T_DR2:
			regNo = 2;
			break;
		case T_DR3:
			regNo = 3;
			break;
		case T_DR6:
			regNo = 6;
			break;
		case T_DR7:
			regNo = 7;
			break;
		/* segments */
		case T_CS:
			regNo = 1;
			break;
		case T_DS:
			regNo = 3;
			break;
		case T_ES:
			regNo = 0;
			break;
		case T_FS:
			regNo = 4;
			break;
		case T_GS:
			regNo = 5;
			break;
		case T_SS:
			regNo = 2;
			break;
		}
	}
	return regNo;
}

/* =====================================================================
  Build up instruction ModRM byte.
  ===================================================================== */
unsigned char BuildModRM(unsigned char modRM, struct Instr_Def* instr, struct expr opnd[4], bool* needModRM, bool* needSIB)
{
	if (instr->flags & F_MODRM)			// Only if the instruction requires a ModRM byte, else return 0.
	{
		*needModRM |= TRUE;
		//  7       5           2       0
		// +---+---+---+---+---+---+---+---+
		// |  mod  |    reg    |    rm     |
		// +---+---+---+---+---+---+---+---+
		// MODRM.mod (2bits) == b11, register to register direct, otherwise register indirect.
		// MODRM.reg (3bits) == 3bit opcode extension, 3bit register value as source. REX.R, VEX.~R can 1bit extend this field.
		// MODRM.rm  (3bits) == 3bit direct or indirect register operand, optionally with displacement. REX.B, VEX.~B can 1bit extend this field.
		if (instr->flags & F_MODRM_REG && instr->op_dir == REG_DST)
		{
			// Build REG field as destination.
			modRM |= (GetRegisterNo(opnd[0].base_reg) & 0x07) << 3;
		}
		else if (instr->flags & F_MODRM_REG && instr->op_dir == RM_DST)
		{
			// Build REG field as source.
			modRM |= (GetRegisterNo(opnd[1].base_reg) & 0x07) << 3;
		}
		if (instr->flags & F_MODRM_RM && instr->op_dir == REG_DST)
		{
			// Build RM field as source.
			modRM |= (GetRegisterNo(opnd[1].base_reg) & 0x07);
		}
		else if (instr->flags & F_MODRM_RM && instr->op_dir == RM_DST)
		{
			// Build RM field as destination.
			modRM |= (GetRegisterNo(opnd[0].base_reg) & 0x07);
		}
	}
	return modRM;
}

/* =====================================================================
  Build up instruction REX prefix byte.
  ===================================================================== */
unsigned char BuildREX(unsigned char RexByte, struct Instr_Def* instr, struct expr opnd[4])
{
	// Only if the identified instruction requires a REX prefix.
	if (((uint_32)instr->flags & (uint_32)REX) != 0)
	{
		/* +---+---+---+---+---+---+---+---+ */
		/* | 0 | 1 | 0 | 0 | W | R | X | B | */
		/* +---+---+---+---+---+---+---+---+ */
		// W == 1=64bit operand size, else default operand size used (usually 32bit).
		// R == extend ModRM.reg
		// X == extend SIB.index
		// B == extend ModRM.rm or SIB.base

		RexByte |= 0x40;		// Fixed base value for REX prefix.
		
		if ((instr->flags & (uint_32)REXB) != 0)
			RexByte |= 0x01;
		if ((instr->flags & (uint_32)REXX) != 0)
			RexByte |= 0x02;
		if ((instr->flags & (uint_32)REXR) != 0)
			RexByte |= 0x04;
		if ((instr->flags & (uint_32)REXW) != 0)
			RexByte |= 0x08;

	}
	/* Instruction promoted with REX.W if specified memory operand is QWORD sized */
	else if ((uint_32)(instr->flags & (uint_32)REXP_MEM) != 0)
	{
		if (ModuleInfo.Ofssize != USE64)
		{
			EmitError(SIGN64_PROMOTION_NOT_POSSIBLE);
		}
		if(SizeFromMemtype(opnd[instr->memOpnd].mem_type, ModuleInfo.Ofssize, opnd[instr->memOpnd].type) == 8)
			RexByte |= 0x48;
	}
	return RexByte;
}

/* =====================================================================
  Build up instruction SIB, ModRM and REX bytes for memory operand.
  ===================================================================== */
int BuildMemoryEncoding(unsigned char* pmodRM, unsigned char* pSIB, unsigned char* pREX, bool* needModRM, bool* needSIB,
	                     unsigned int* dispSize, uint_64* pDisp, struct Instr_Def* instr, struct expr opExpr[4]) 
{
	int             returnASO   = 0;
	unsigned char   sibScale    = 0;
	uint_32         memModeIdx  = 0;
	unsigned char   baseRegNo   = 17; 
	unsigned char   idxRegNo    = 17;
	int             baseRegSize = 0;
	int             idxRegSize  = 0;
	int             symSize     = 0;
	bool			skipSIB     = FALSE;

	/* Absolute addressing modes can skip this */
	if(instr->memOpnd != MEM_ABS_1)
	{ 
		baseRegNo = GetRegisterNo(opExpr[instr->memOpnd].base_reg);
		idxRegNo  = GetRegisterNo(opExpr[instr->memOpnd].idx_reg);

		/* Get base and index register sizes in bytes */
		if (opExpr[instr->memOpnd].base_reg)
			baseRegSize = SizeFromRegister(opExpr[instr->memOpnd].base_reg->tokval);
		if (opExpr[instr->memOpnd].idx_reg)
			idxRegSize = SizeFromRegister(opExpr[instr->memOpnd].idx_reg->tokval);
	}

	/* Base and Index registers must be of the same size (except for VSIB) */
	if (baseRegSize != idxRegSize && idxRegSize < 16 && idxRegSize > 0 && baseRegSize > 0)
	{
		EmitError(BASE_INDEX_MEMORY_SIZE_ERROR);
		return returnASO;
	}

	/* 16bit Memory Addressing is not allowed in long mode */
	if (ModuleInfo.Ofssize == USE64 && baseRegSize == 2)
	{
		EmitError(BITS16_MEM_NOT_ALLOWED_IN_LONG_MODE);
		return returnASO;
	}

	/* If the memory address refers to a symbol indirectly.. */
	if (opExpr[instr->memOpnd].sym && opExpr[instr->memOpnd].kind == EXPR_ADDR) 
	{
		symSize = SizeFromMemtype(opExpr[instr->memOpnd].mem_type, ModuleInfo.Ofssize, opExpr[instr->memOpnd].sym);
		
		if(ModuleInfo.Ofssize == USE64 && opExpr[instr->memOpnd].sym->state != SYM_STACK)
			baseRegNo = 16; // For 64bit mode, all symbol references are RIP relative, unless the symbol is on the stack.

	}
	else
	{
		/* Address format requires Address Size Override Prefix */
		if ((ModuleInfo.Ofssize == USE64 && baseRegSize == 4) ||
			(ModuleInfo.Ofssize == USE32 && baseRegSize == 2) ||
			(ModuleInfo.Ofssize == USE16 && baseRegSize == 4))
			returnASO = 1;
	}

	/* Calculate 16bit memory addressing ModRM in 32bit mode */
	if (ModuleInfo.Ofssize == USE32 && baseRegSize == 2)
	{
		if (instr->memOpnd < NO_MEM)
		{
			//[BX + SI]	[BX + DI]	[BP + SI]	[BP + DI]	[SI]	[DI]	[disp16]	[BX]
			if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BX && opExpr[instr->memOpnd].idx_reg->tokval == T_SI)
				*pmodRM |= 0;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BX && opExpr[instr->memOpnd].idx_reg->tokval == T_DI)
				*pmodRM |= 1;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BP && opExpr[instr->memOpnd].idx_reg->tokval == T_SI)
				*pmodRM |= 2;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].idx_reg &&
				opExpr[instr->memOpnd].base_reg->tokval == T_BP && opExpr[instr->memOpnd].idx_reg->tokval == T_DI)
				*pmodRM |= 3;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->tokval == T_SI)
				*pmodRM |= 4;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->tokval == T_DI)
				*pmodRM |= 5;
			else if (!opExpr[instr->memOpnd].base_reg && !opExpr[instr->memOpnd].idx_reg)
				*pmodRM |= 6;
			else if (opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->tokval == T_BX)
				*pmodRM |= 7;

			*needModRM |= TRUE;

			skipSIB = TRUE;
		}
	}
	else
	{
		/* Use the memory encoding table to populate the initial modRM, sib values */
		/* ----------------------------------------------------------------------- */
		memModeIdx = (baseRegNo * 18) + (idxRegNo);

		if (instr->memOpnd < NO_MEM) // MEM_ABS_x for moffset type addresses don't need to be processed.
		{
			// Addressing form cannot be encoded.
			if (MemTable[memModeIdx].flags & NO_ENCODE)
			{
				EmitError(INVALID_ADDRESSING_MODE_WITH_CURRENT_CPU_SETTING);
				return returnASO;
			}

			// Setup ModRM.
			if (MemTable[memModeIdx].flags & MEMF_MODRM)
			{
				*pmodRM |= MemTable[memModeIdx].modRM;
				*needModRM |= TRUE;
			}
			// Setup SIB.
			if (MemTable[memModeIdx].flags & MEMF_SIB)
			{
				*pSIB |= MemTable[memModeIdx].SIB;
				*needSIB |= TRUE;
			}
		}
	}

	// Does the memory address require a displacement?
	/* ----------------------------------------------------------------------- */
	// Either: User specified
	//         RIP relative addressing mandates it
	//         Specified address format can only be encoded with a displacement.
	if (instr->memOpnd > NO_MEM || opExpr[instr->memOpnd].value != 0 || 
		(ModuleInfo.Ofssize == USE64 && opExpr[instr->memOpnd].base_reg && opExpr[instr->memOpnd].base_reg->token == T_RIP) ||
		MemTable[memModeIdx].flags & MEMF_DSP || MemTable[memModeIdx].flags & MEMF_DSP32 || opExpr[instr->memOpnd].sym)
	{
		if (instr->memOpnd > NO_MEM)
		{
			switch (ModuleInfo.Ofssize)
			{
			case USE16:
				*dispSize = 2;
				break;
			case USE32:
				*dispSize = 4;
				break;
			case USE64:
				*dispSize = 8;
				break;
			}
		}
		else
		{
			// Is it 8bit or 16/32bit (RIP only allows 32bit)?
			if (!opExpr[instr->memOpnd].sym && (((MemTable[memModeIdx].flags & MEMF_DSP32) == 0) && (opExpr[instr->memOpnd].value >= -128 && opExpr[instr->memOpnd].value <= 127)) || (MemTable[memModeIdx].flags & MEMF_DSP))
			{
				*dispSize = 1;
				*pmodRM |= MODRM_DISP8;
			}
			else
			{
				if (ModuleInfo.Ofssize == USE16)
					*dispSize = 2;	// 16bit addressing.
				else
					*dispSize = 4;	// 32bit or 64bit sign extended addressing.

				if ((int)(MemTable[memModeIdx].flags & MEMF_DSP32) == 0)
					*pmodRM |= MODRM_DISP;
			}
		}
		*pDisp = opExpr[(instr->memOpnd & 7)].value64;
	}

	// Extend REX(.B) and REX(.X) to account for 64bit base and index registers.
	// We use RegNo==16 to represent RIP (even though it's not directly encodable).
	/* ----------------------------------------------------------------------- */
	if (baseRegNo > 7 && baseRegNo < 16) 
		*pREX |= 0x41;
	if (idxRegNo > 7 && idxRegNo < 16)
		*pREX |= 0x42;

	//  scale index    base
	// +--+--+--+--+--+--+--+--+
	// | 7   | 5      | 2      |
	// +--+--+--+--+--+--+--+--+
	// SIB.scale == 0,1,2,3 = (1,2,4,8) scale factor.
	// SIB.index == index register to use, extended via REX.X or VEX.~X
	// SIB.base  == base register to use, extended via REX.B or VEX.~B

	/* (E/R)SP or R12 or presence of scale or index register requires SIB addressing modes */
	/* ----------------------------------------------------------------------- */
	if (!skipSIB && instr->memOpnd < NO_MEM && (opExpr[instr->memOpnd].scale > 1 || opExpr[instr->memOpnd].idx_reg != 0 || (MemTable[memModeIdx].flags & MEMF_SIB)))
	{
		switch (opExpr[instr->memOpnd].scale)
		{
		case 1:
			sibScale = 0;
			break;
		case 2:
			sibScale = 1;
			break;
		case 4:
			sibScale = 2;
			break;
		case 8:
			sibScale = 3;
			break;
		}
		*pSIB |= (sibScale << 6);
		*needSIB |= TRUE;
	}

	return returnASO;
}

ret_code CodeGenV2(const char* instr, struct code_info *CodeInfo, uint_32 oldofs, uint_32 opCount, struct expr opExpr[4])
{
	struct Instr_Def  instrToMatch;
	ret_code          retcode      = NOT_ERROR;
	struct Instr_Def* matchedInstr = NULL;
	uint_32           i            = 0;

	bool needModRM = FALSE;
	bool needSIB   = FALSE;
	bool needFixup = FALSE;
	bool hasMemReg = FALSE;
	int  aso       = 0; /* Build Memory Encoding forced address size override */

	unsigned char opcodeByte = 0;
	unsigned char rexByte    = 0;
	unsigned char modRM      = 0;
	unsigned char sib        = 0;
	unsigned int  dispSize   = 0;
	
	union
	{
		uint_64 displacement64;
		unsigned char byte[8];
	} displacement;

	union
	{
		uint_64 full;
		unsigned char byte[8];
	} immValue;

	/* Determine which Memory Encoding Format Table to Use. */
	if (CodeInfo->Ofssize == USE64)
		MemTable = &MemTable64;
	else
		MemTable = &MemTable32;

	/* Force JWASM style FLAT: override back to legacy CodeGen. */
	if (opExpr[1].override && opExpr[1].override->tokval == T_FLAT)
		return EMPTY;
	
	//return EMPTY; // Uncomment this to disable new CodeGenV2.

	memset(&instrToMatch, 0, sizeof(struct Instr_Def));
	instrToMatch.mnemonic      = instr;		/* Instruction mnemonic string */
	instrToMatch.operand_count = opCount;	/* Number of operands */
	/* Translate to CodeGenV2 operand types */
	for (i = 0; i < opCount; i++)
	{
		instrToMatch.operand_types[i] = MatchOperand(CodeInfo, CodeInfo->opnd[i], opExpr[i]);
		/* Determine if we have a memory operand, if it contains registers */
		if (opExpr[i].kind == EXPR_ADDR && (opExpr[i].base_reg || opExpr[i].idx_reg))
			hasMemReg = TRUE;
		/* Is indirect with reg (this would indicate RIP) */
		if (opExpr[i].kind == EXPR_REG && opExpr[i].indirect)
			hasMemReg = TRUE;
		/* Refers to a symbol in 64bit mode (in which case a RIP relative mode is better) */
		if (CodeInfo->Ofssize == USE64 && opExpr[i].sym)
			hasMemReg = TRUE;
	}
	
	/* Lookup the instruction */
	matchedInstr = LookupInstruction(&instrToMatch, hasMemReg);

	/* Try once again with demoted operands */
	if (matchedInstr == NULL)
	{
		for (i = 0; i < opCount; i++)
			instrToMatch.operand_types[i] = DemoteOperand(instrToMatch.operand_types[i]);
		matchedInstr = LookupInstruction(&instrToMatch, hasMemReg);
	}

	/* We don't have it in CodeGenV2 so fall-back */
	if(matchedInstr == NULL)
		retcode = EMPTY;

	/* Proceed to generate the instruction */
	else
	{
		//----------------------------------------------------------
		// Add line number debugging info.
		//----------------------------------------------------------
		if (Options.line_numbers)
			AddLinnumDataRef(get_curr_srcfile(), GetLineNumber());

		//----------------------------------------------------------
		// Check if instruction is valid in current mode.
		//----------------------------------------------------------
		if (!IsValidInCPUMode(matchedInstr))
		{
			EmitError(INSTRUCTION_OR_REGISTER_NOT_ACCEPTED_IN_CURRENT_CPU_MODE);
			return;
		}
	
		//----------------------------------------------------------
		// Decide if a fixup is required.
		// -> A fixup is only required if a memory opnd is used
		// -> And only then if it refers to a symbol
		//----------------------------------------------------------
		if (matchedInstr->memOpnd != NO_MEM)
		{
			if (opExpr[matchedInstr->memOpnd].sym)
				needFixup = TRUE;
		}
		if (CodeInfo->opnd[OPND2].InsFixup)
			needFixup = TRUE;

		//----------------------------------------------------------
		// Build Memory Encoding Format.
		// -> When an indirect memory operand is used, this will build
		// -> up the respective rex, modrm and sib values.
		// Alternatively directly encode the modRM, rex and SIB values.
		//----------------------------------------------------------
		/* If the matched instruction requires processing of a memory address */
		if(matchedInstr->memOpnd != NO_MEM)
			aso = BuildMemoryEncoding(&modRM, &sib, &rexByte, &needModRM, &needSIB,
				                &dispSize, &displacement, matchedInstr, opExpr);					/* This could result in modifications to REX, modRM and SIB bytes */
		modRM   |= BuildModRM(matchedInstr->modRM, matchedInstr, opExpr, &needModRM, &needSIB);		/* Modify the modRM value for any non-memory operands */
		if(CodeInfo->Ofssize == USE64)
			rexByte |= BuildREX(rexByte, matchedInstr, opExpr);									    /* Modify the REX prefix for non-memory operands/sizing */

		//----------------------------------------------------------
		// Check if address or operand size override prefixes are required.
		//----------------------------------------------------------
		if (Require_ADDR_Size_Override(matchedInstr, CodeInfo) || aso)
			OutputCodeByte(ADDR_SIZE_OVERRIDE);
		if (Require_OPND_Size_Override(matchedInstr, CodeInfo))
			OutputCodeByte(OP_SIZE_OVERRIDE);

		//----------------------------------------------------------
		// Output Segment Prefix if required and allowed.
		//----------------------------------------------------------
		if (CodeInfo->prefix.RegOverride != ASSUME_NOTHING)
		{
			if (matchedInstr->flags & ALLOW_SEG)
			{
				if (ModuleInfo.Ofssize == USE64 && (CodeInfo->prefix.RegOverride == ASSUME_FS || CodeInfo->prefix.RegOverride == ASSUME_GS || CodeInfo->prefix.RegOverride == ASSUME_SS))
				{
					switch (CodeInfo->prefix.RegOverride)
					{
					case ASSUME_FS:
						OutputCodeByte(PREFIX_FS);
						break;
					case ASSUME_GS:
						OutputCodeByte(PREFIX_GS);
						break;
					case ASSUME_SS:
						OutputCodeByte(PREFIX_SS);
						break;
					}
				}
				else if (ModuleInfo.Ofssize == USE64 && ((matchedInstr->flags & ALLOW_SEGX) == 0))
				{
					EmitError(ILLEGAL_USE_OF_SEGMENT_REGISTER);
					return;
				}
				else
				{
					switch (CodeInfo->prefix.RegOverride)
					{
					case ASSUME_CS:
						OutputCodeByte(PREFIX_CS);
						break;
					case ASSUME_DS:
						OutputCodeByte(PREFIX_DS);
						break;
					case ASSUME_ES:
						OutputCodeByte(PREFIX_ES);
						break;
					case ASSUME_SS:
						OutputCodeByte(PREFIX_SS);
						break;
					case ASSUME_FS:
						OutputCodeByte(PREFIX_FS);
						break;
					case ASSUME_GS:
						OutputCodeByte(PREFIX_GS);
						break;
					}
				}
			}
			else
			{
				EmitError(ILLEGAL_USE_OF_SEGMENT_REGISTER);
				return;
			}
		}

		//----------------------------------------------------------
		// Validate and output other prefixes (LOCK,REPx, BND)
		//----------------------------------------------------------
		if (CodeInfo->prefix.ins == T_BND && (matchedInstr->flags & ALLOW_BND) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_BND)
			OutputCodeByte(BND);
		if (CodeInfo->prefix.ins == T_LOCK && (matchedInstr->flags & ALLOW_LOCK) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_LOCK)
			OutputCodeByte(LOCK);
		if (CodeInfo->prefix.ins == T_REP && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REP)
			OutputCodeByte(REP);
		if (CodeInfo->prefix.ins == T_REPE && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPE)
			OutputCodeByte(REPE);
		if (CodeInfo->prefix.ins == T_REPZ && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPZ)
			OutputCodeByte(REPZ);
		if (CodeInfo->prefix.ins == T_REPNE && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPNE)
			OutputCodeByte(REPNE);
		if (CodeInfo->prefix.ins == T_REPNZ && (matchedInstr->flags & ALLOW_REP) == 0)
			EmitError(INSTRUCTION_PREFIX_NOT_ALLOWED);
		else if (CodeInfo->prefix.ins == T_REPNZ)
			OutputCodeByte(REPNZ);

		//----------------------------------------------------------
		// Output FPU FWAIT if required.
		//----------------------------------------------------------

		//----------------------------------------------------------
		// Output mandatory prefix.
		//----------------------------------------------------------
		if (matchedInstr->mandatory_prefix != 0)
		{
			
		}

		//----------------------------------------------------------
		// Output VEX prefix if required.
		//----------------------------------------------------------

		//----------------------------------------------------------
		// Output REX prefix if required.
		//----------------------------------------------------------
		if (rexByte != 0)
			OutputCodeByte(rexByte);

		//----------------------------------------------------------
		// Output opcode byte(s).
		//----------------------------------------------------------
		// Single opcode byte with embedded register.
		if ((matchedInstr->flags & F_OPCODE_REG) != 0)
		{
			opcodeByte = matchedInstr->opcode[0];
			opcodeByte += (GetRegisterNo(opExpr[0].base_reg) & 0x07);
			OutputCodeByte(opcodeByte);
		}
		// Normal opcode byte sequence.
		else
		{
			for (i = 0; i < matchedInstr->opcode_bytes; i++)
				OutputCodeByte(matchedInstr->opcode[i]);
		}

		//----------------------------------------------------------
		// Output ModR/M
		//----------------------------------------------------------
		if (needModRM)
			OutputCodeByte(modRM);

		//----------------------------------------------------------
		// Output SIB
		//----------------------------------------------------------
		if (needSIB)
			OutputCodeByte(sib);

		//----------------------------------------------------------
		//  Output Displacement and Fixup.
		//----------------------------------------------------------
		if (dispSize) 
		{
			if (CodeInfo->opnd[matchedInstr->memOpnd].InsFixup && needFixup)
			{
				if (Parse_Pass > PASS_1)
					if ((1 << CodeInfo->opnd[matchedInstr->memOpnd].InsFixup->type) & ModuleInfo.fmtopt->invalid_fixup_type) 
					{
						EmitErr(UNSUPPORTED_FIXUP_TYPE, ModuleInfo.fmtopt->formatname, CodeInfo->opnd[matchedInstr->memOpnd].InsFixup->sym ? CodeInfo->opnd[matchedInstr->memOpnd].InsFixup->sym->name : szNullStr);
					}
				if (write_to_file) 
				{
					CodeInfo->opnd[matchedInstr->memOpnd].InsFixup->locofs = GetCurrOffset();
					if (CodeInfo->isptr)
						OutputBytes((unsigned char *)&displacement.byte, dispSize, NULL);
					else
						OutputBytes((unsigned char *)&displacement.byte, dispSize, CodeInfo->opnd[matchedInstr->memOpnd].InsFixup);
				}
			}
		}

		//----------------------------------------------------------
		// Output Immediate Data.
		//----------------------------------------------------------
		if (matchedInstr->immOpnd != NO_IMM)
		{
			immValue.full = CodeInfo->opnd[matchedInstr->immOpnd].data64;
			// An immediate entry could require a fixup if it was generated from an OFFSET directive or Symbol value.
			if (CodeInfo->opnd[matchedInstr->immOpnd].InsFixup && needFixup)
			{
				if (Parse_Pass > PASS_1)
					if ((1 << CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->type) & ModuleInfo.fmtopt->invalid_fixup_type)
					{
						EmitErr(UNSUPPORTED_FIXUP_TYPE, ModuleInfo.fmtopt->formatname, CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->sym ? CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->sym->name : szNullStr);
					}
				//			if (dispSize == 0 && needFixup)
				//			{
				if (write_to_file)
				{
					CodeInfo->opnd[matchedInstr->immOpnd].InsFixup->locofs = GetCurrOffset();
					if (CodeInfo->isptr)
						OutputBytes((unsigned char *)&immValue.byte, matchedInstr->op_size, NULL);
					else
						OutputBytes((unsigned char *)&immValue.byte, matchedInstr->op_size, CodeInfo->opnd[matchedInstr->immOpnd].InsFixup);
				}
				//		}
					//	else 
						//	OutputBytes((unsigned char *)&immValue.byte, matchedInstr->op_size, NULL);
			}
		}

		//----------------------------------------------------------
		// Finalize fixup post immediate data.
		//----------------------------------------------------------
		//if (needFixup)
		//{
			// For rip-relative fixups, the instruction end is needed
			if (CodeInfo->Ofssize == USE64)
			{
				if (CodeInfo->opnd[OPND1].InsFixup && CodeInfo->opnd[OPND1].InsFixup->type == FIX_RELOFF32)
					CodeInfo->opnd[OPND1].InsFixup->addbytes = GetCurrOffset() - CodeInfo->opnd[OPND1].InsFixup->locofs;
				if (CodeInfo->opnd[OPND2].InsFixup && CodeInfo->opnd[OPND2].InsFixup->type == FIX_RELOFF32)
					CodeInfo->opnd[OPND2].InsFixup->addbytes = GetCurrOffset() - CodeInfo->opnd[OPND2].InsFixup->locofs;
			}
		//}
	}

	// Write out listing.
	if (retcode == NOT_ERROR)
	{
		if (CurrFile[LST])
			LstWrite(LSTTYPE_CODE, oldofs, NULL);
	}

	return retcode;
}

//  /c /DX86_64 /DWINDOWS64 -win64 /DARCH=SKX -Fl=out.lst /Foamd64\zr4dwpn_skx.obj zr4dwpn.asm
//  d:\foo2