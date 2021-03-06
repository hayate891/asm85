// asm85 - an 8085 assembler
//
// Copyright (c) 2016 Tom Nisbet
// Licensed under the MIT license

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>

#include "imagestore.h"
#include "instructions.h"
#include "scanner.h"
#include "symboltable.h"

SymbolTable symbols;

extern InstructionEntry instructionTable[];


////////////////////////////////////////////////////////////////////////////////
class AsmLine
{
  public:
    enum
    {
        // Return codes from the processing routines.
        RET_OK,             // Normal processing, data may be present
        RET_DIR_EQU,        // EQU directive, value in auxValue
        RET_DIR_DS,         // DS directive, size in auxValue
        RET_NOTHING_DONE,   // No processing done for this line
        RET_WARNING,        // Warning condition detected
        RET_ERROR,          // Error condition detected
        RET_PASS1_ERROR     // Error that is only detected at pass 1.
    };
    enum
    {
        BUFFER_SIZE = 256,
        SMALL_BUFFER_SIZE = 32,
        EXPR_ERROR = 0x80000000     // Error value for Expression Evaluators.
    };

    AsmLine();
    int Process(const char * line, uint16_t addr, int pass);

    const char * GetErrorMsg() { return errorMsg; }
    const uint8_t * GetBytes() { return bytes; }
    const int GetNumBytes() { return numBytes; }
    uint16_t GetStartAddr() { return startAddr; }
    uint16_t GetAuxValue() { return auxValue; }

  private:
    char        errorMsg[BUFFER_SIZE];
    uint8_t     bytes[BUFFER_SIZE];
    int         numBytes;
    uint16_t    startAddr;
    uint16_t    auxValue;
    Scanner     scanner;
    int         pass;
    char *      pLabel;
    char *      pMnemonic;

    int Failure(int status, const char * pMsg, const char * pParam = NULL);
    int ProcessDirective();
    int ProcessInstruction();
    int StoreArgList(int size);

    // Recursive descent expression evaluation
    unsigned EvaluateAtom();
    unsigned EvaluateFactors();
    unsigned EvaluateSums();
    unsigned EvaluateBitwiseAnd();
    unsigned EvaluateBitwiseOr();
    unsigned EvaluateExpression();
};


AsmLine::AsmLine()
{
    pLabel = pMnemonic = NULL;
}


int AsmLine::Process(const char * line, uint16_t addr, int ps)
{
    strcpy(errorMsg, "asm85");
    startAddr = addr;
    pass = ps;
    numBytes = 0;

    if (pLabel)
    {
        free(pLabel);
        pLabel = NULL;
    }
    if (pMnemonic)
    {
        free(pMnemonic);
        pMnemonic = NULL;
    }

    int t = scanner.Init(line);
    if (t == Scanner::T_LABEL)
    {
        pLabel = strdup(scanner.GetString());
        t = scanner.Next();
    }
    if (t == Scanner::T_IDENTIFIER)
    {
        pMnemonic = strdup(scanner.GetString());
        scanner.Next();
    }
//    else if ((t != Scanner::T_EOF) && (t != Scanner::T_ERROR))
    else if (t == Scanner::T_ERROR)
    {
        return Failure(RET_ERROR, scanner.GetErrorMsg());
    }

    int status = RET_NOTHING_DONE;
    if (pLabel && (pass == 1))
    {
        status = symbols.Add(pLabel, startAddr);
        if (status == SymbolTable::RET_DUPLICATE)
        {
            return Failure(RET_PASS1_ERROR, "Symbol defined more than once", pLabel);
        }
    }
    if (pMnemonic == NULL)
    {
        return RET_NOTHING_DONE;
    }

    status = ProcessDirective();
    if (status == RET_NOTHING_DONE)
    {
        status = ProcessInstruction();
    }

    return status;
}


int AsmLine::Failure(int status, const char * pMsg, const char * pParam)
{
    if (pParam)
    {
        sprintf(errorMsg, "%s: %s", pMsg, pParam);
    }
    else
    {
        sprintf(errorMsg, "%s", pMsg);
    }

    return status;
}


int AsmLine::ProcessDirective()
{
    int status = RET_NOTHING_DONE;

    if (strcasecmp("ORG", pMnemonic) == 0)
    {
        unsigned val = EvaluateExpression();
        if (val == EXPR_ERROR)
        {
            // Error message provided by Evaluate.
            return RET_ERROR;
        }
        startAddr = val;
        auxValue = uint16_t(val);
        return RET_DIR_EQU; // Same output as EQU
    }
    else if (strcasecmp("EQU", pMnemonic) == 0)
    {
        if (pLabel == NULL)
        {
            return Failure(RET_ERROR, "No label for EQU directive");
        }
        else
        {
            unsigned val = EvaluateExpression();
            if (val == EXPR_ERROR)
            {
                // Error message provided by Evaluate.
                return RET_ERROR;
            }
            status = symbols.Update(pLabel, val);
            if (status != SymbolTable::RET_OK)
            {
                // Shouldn't happen because label was added just before this call.
                return Failure(RET_ERROR, "could not update symbol", pLabel);
            }
            auxValue = uint16_t(val);
            return RET_DIR_EQU;
        }
    }

    else if (strcasecmp("DB", pMnemonic) == 0)
    {
        return StoreArgList(1);
    }

    else if (strcasecmp("DW", pMnemonic) == 0)
    {
        return StoreArgList(2);
    }

    else if (strcasecmp("DS", pMnemonic) == 0)
    {
        unsigned val = EvaluateExpression();
        if (val == EXPR_ERROR)
        {
            // Error message provided by Evaluate.
            return RET_ERROR;
        }
        auxValue = uint16_t(val);
        return RET_DIR_DS;
    }

    else if (strcasecmp("END", pMnemonic) == 0)
    {
        // This doesn't really do anything.  Just mark it as processed so
        // it does not get interpreted as a processor instruction.
        return RET_OK;
    }

    // Signal that no assembler directive was found so this line should be
    // processed as a machine instruction.
    return RET_NOTHING_DONE;
}


int AsmLine::ProcessInstruction()
{
    enum { ST_SEARCHING, ST_NOT_FOUND, ST_SUCCESS } state = ST_SEARCHING;
    bool bMnemonicFound = false;
    bool bSecondArg = false;
    int numRegs = 0;
    char reg1[4];
    char reg2[4];
    reg1[0] = '\0';
    reg2[0] = '\0';

    // Handle the RST instructions as a special case because the instructions
    // are differentiated by a numeric argument instead of a register name.
    if (strcasecmp(pMnemonic, "RST") == 0)
    {
        const char * arg = scanner.GetString();
        if ((scanner.GetType() != Scanner::T_CONSTANT) ||
            (strlen(arg) != 1) || (*arg < '0') || (*arg > '7'))
        {
            return Failure(RET_ERROR, "RST instruction argument must be 0-7", arg);
        }
        bytes[numBytes++] = 0xc7 | ((*arg - '0') << 3);
        if (scanner.Next() != Scanner::T_EOF)
        {
            return Failure(RET_ERROR, "Found extra arguments after RST instruction:", scanner.GetString());
        }
        return RET_OK;
    }

    // Look for <register> or <register>,<register> and set the register count.
    if (scanner.GetType() == Scanner::T_REGISTER)
    {
        strcpy(reg1, scanner.GetString());
        ++numRegs;
        if (scanner.PeekChar() == ',')
        {
            // If the next token is a comma, then a second argument is
            // present that may be a register or the start of an expression.
            scanner.Next(); // Get the comma
            scanner.Next(); // Register or start of expr
            if (scanner.GetType() == Scanner::T_REGISTER)
            {
                strcpy(reg2, scanner.GetString());
                ++numRegs;
            }
            else
            {
                // Take note of the second, non-register arg for error check.
                bSecondArg = true;
            }
        }
    }

    for (int ix = 0; (state == ST_SEARCHING); ix++)
    {
        InstructionEntry * pInst = instructionTable + ix;
        int cmp = strcasecmp(pInst->mnemonic, pMnemonic);
        if (cmp == 0)
        {
//printf("Inst=[%02x  %4s %s,%s  %d],  found=[%d, %s, %s]\n",
//       pInst->opcode, pInst->mnemonic, pInst->reg1, pInst->reg2, pInst->nRegs,
//       numRegs, reg1, reg2);
            bMnemonicFound = true;
            if (pInst->nRegs != numRegs)
            {
                return Failure(RET_ERROR, "Wrong number of register arguments for instruction", pMnemonic);
            }
            if ((pInst->nRegs >= 1) && (strcasecmp(pInst->reg1, reg1) != 0))  continue;
            if ((pInst->nRegs == 2) && (strcasecmp(pInst->reg2, reg2) != 0))  continue;
            bytes[numBytes++] = pInst->opcode;

            // Process additional argument, if needed.
            if (pInst->argType == EX_NONE)
            {
                if (!bSecondArg)
                {
                    // If a non-register arg was not seen, the current token
                    // is the last register.  Scan past it for the EOL check.
                    scanner.Next();
                }
            }
            else
            {
                // Instruction requires an expression argument.
                unsigned val = EvaluateExpression();
                if (val == EXPR_ERROR)
                {
                    // Error message handled by expression.
                    return RET_ERROR;
                }
                bytes[numBytes++] = val & 0xff;
                if (pInst->argType == EX_WORD)
                {
                    bytes[numBytes++] = val >> 8;
                }
            }
            state = ST_SUCCESS;
        }
        else if (cmp > 0)
        {
            // Mnemonics are alpha order, so stop searching.
            state = ST_NOT_FOUND;
        }
    }

    if (state == ST_SUCCESS)
    {
        if (scanner.GetType() != Scanner::T_EOF)
        {
            return Failure(RET_ERROR, "Additional arguments after instruction",
                           scanner.GetString());
        }
        return RET_OK;
    }
    else if (bMnemonicFound)
    {
        return Failure(RET_ERROR, "Wrong arguments for instruction", pMnemonic);
    }

    return Failure(RET_ERROR, "No instruction with this name", pMnemonic);
}


// Store one or more arguments as a sequence or bytes or words.  Arguments can
// be identifiers, numeric constants, or expressions.  A multi-character string
// is interpreted as a sequence of bytes, so the string "ABC" is equivalent
// to "A","B","C".  A single character string can be used in an expression as
// a numeric constant, like "A" | 020H.  Single or double quotes are allowed.
int AsmLine::StoreArgList(int size)
{
    unsigned val;
    bool bMore = true;

    while (bMore)
    {
        if ((scanner.GetType() == Scanner::T_STRING) && (scanner.GetLength() > 1))
        {
            // Strings of length greater than one are stored as a series of
            // bytes.  Single character strings may be treated as a single
            // byte or as a numeric part of a larger expression.
            memcpy(bytes + numBytes, scanner.GetBuffer(), scanner.GetLength());
            numBytes += scanner.GetLength();
            scanner.Next();
        }
        else
        {
            val = EvaluateExpression();
            if (val == EXPR_ERROR)
            {
                // Error message provided by Evaluate.
                return RET_ERROR;
            }
            else
            {
                if (size == 1)
                {
                    // Store a byte
                    bytes[numBytes++] = val & 0xff;
                }
                else
                {
                    // Store a word
                    bytes[numBytes++] = val & 0xff;
                    bytes[numBytes++] = val >> 8;
                }
            }
        }
        bMore = (scanner.GetType() == Scanner::T_COMMA);
        scanner.Next();
    }

    if (scanner.GetType() != Scanner::T_EOF)
    {
        return Failure(RET_WARNING, "Found additonal characters after expression list:",
                       scanner.GetString());
    }
    return RET_OK;
}


unsigned AsmLine::EvaluateAtom()
{
    unsigned val;

    int t = scanner.GetType();
    if (t == Scanner::T_ERROR)
    {
        Failure(RET_ERROR, scanner.GetErrorMsg());
        val = EXPR_ERROR;
    }

    else if (t == Scanner::T_OPEN_PAREN)
    {
        scanner.Next();
        val = EvaluateExpression();
        if (val == EXPR_ERROR)  return EXPR_ERROR;
        if (scanner.GetType() != Scanner::T_CLOSE_PAREN)
        {
            Failure(RET_ERROR, "Expecting close parenthesis, found", scanner.GetString());
            val = EXPR_ERROR;
        }
    }
    else if (t == Scanner::T_DOLLAR)
    {
        val = startAddr;
    }

    else if (t == Scanner::T_CONSTANT)
    {
        val = scanner.GetValue();
    }

    else if (t == Scanner::T_STRING)
    {
        if (scanner.GetLength() == 1)
        {
            // Treat a one character string as a numeric constant, like a single
            // quoted char in C.  For example, 'A' == 65.
            val = *scanner.GetString();
        }
        else
        {
            Failure(RET_ERROR, "Multi-character string not allowed in expression.");
            val = EXPR_ERROR;
        }
    }
    else if (t == Scanner::T_IDENTIFIER)
    {
        // Try the symbol table.
        val = symbols.Lookup(scanner.GetString());
        if (val == SymbolTable::NO_ENTRY)
        {
            if (pass > 1)
            {
                Failure(RET_ERROR, "Label not found", scanner.GetString());
                val = EXPR_ERROR;
            }
            else
            {
                // Ignore the error in the first pass of assembly.  It might be
                // a reference to a symbol that isn't defined yet.
                val = 0;
            }
        }
    }

    else
    {
        Failure(RET_ERROR, "Expected label or numeric constant, found:", scanner.GetString());
        val = EXPR_ERROR;
    }

    scanner.Next();
    return val;
}


// Parse multiplication and division
unsigned AsmLine::EvaluateFactors()
{
    unsigned num1 = EvaluateAtom();
    if (num1 == EXPR_ERROR)  return EXPR_ERROR;
    while (scanner.GetType() == Scanner::T_FACTOR_OPER)
    {
        int op = scanner.GetValue();
        scanner.Next();
        unsigned num2 = EvaluateAtom();
        if (num2 == EXPR_ERROR)  return EXPR_ERROR;
        if (op == Scanner::V_DIVIDE)
        {
            if (num2 == 0)
            {
                Failure(RET_ERROR, "Divide by zero");
                return EXPR_ERROR;
            }
            num1 /= num2;
        }
        else if (op == Scanner::V_MOD)
        {
            num1 %= num2;
        }
        else
        {
            num1 *= num2;
        }
    }

    return num1;
}


// Parse addition and subtraction
unsigned AsmLine::EvaluateSums()
{
    unsigned num1 = EvaluateFactors();
    if (num1 == EXPR_ERROR)  return EXPR_ERROR;
    while (scanner.GetType() == Scanner::T_SUM_OPER)
    {
        int op = scanner.GetValue();
        scanner.Next();
        unsigned num2 = EvaluateFactors();
        if (num2 == EXPR_ERROR)  return EXPR_ERROR;
        if (op == Scanner::V_MINUS)
        {
            num1 -= num2;
        }
        else
        {
            num1 += num2;
        }
    }
    return num1;
}


// Evaluate bitwise AND expression
unsigned AsmLine::EvaluateBitwiseAnd()
{
    unsigned num1 = EvaluateSums();
    if (num1 == EXPR_ERROR)  return EXPR_ERROR;
    while (scanner.GetType() == Scanner::T_BIT_AND_OPER)
    {
        scanner.Next();
        unsigned num2 = EvaluateSums();
        if (num2 == EXPR_ERROR)  return EXPR_ERROR;
        num1 &= num2;
    }

    return num1;
}


// Evaluate bitwise OR expression
unsigned AsmLine::EvaluateBitwiseOr()
{
    unsigned num1 = EvaluateBitwiseAnd();
    if (num1 == EXPR_ERROR)  return EXPR_ERROR;
    while (scanner.GetType() == Scanner::T_BIT_OR_OPER)
    {
        int op = scanner.GetValue();
        scanner.Next();
        unsigned num2 = EvaluateBitwiseAnd();
        if (num2 == EXPR_ERROR)  return EXPR_ERROR;
        if (op == Scanner::V_OR)
        {
            num1 |= num2;
        }
        else
        {
            num1 ^= num2;
        }
    }

    return num1;
}


unsigned AsmLine::EvaluateExpression()
{
    return EvaluateBitwiseOr();
}



////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
int main(int argc, char * argv[])
{
    char line[256];
    char * asmName;
    char * listName;
    char * hexName;
    char * binName;
    FILE * asmFile;
    FILE * listFile;
    FILE * hexFile;
    FILE * binFile;
    unsigned addr = 0;
    ImageStore image(65536);
    char * binAddrs[100];
    int numBins = 0;
    int c;

    opterr = 0;
    while ((c = getopt (argc, argv, "b:")) != -1)
    {
        switch (c)
        {
        case 'b':
            if ((strlen(optarg) != 9) || (optarg[4] != ':') ||
                (strspn(optarg, "0123456789abcdefABCDEF:") != 9))
            {
                fprintf(stderr,
                        "option for -b must be hex start and end in the form: ssss:eeee\n");
                abort();
            }
            binAddrs[numBins++] = strdup(optarg);
            break;
        case '?':
            if (optopt == 'b')
                fprintf (stderr, "Option -%c requires an argument.\n", optopt);
            else if (isprint (optopt))
                fprintf (stderr, "Unknown option `-%c'.\n", optopt);
            else
                fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
            return 1;
        default:
            abort ();
        }
    }

    if (optind != argc - 1)
    {
        fprintf(stderr, "usage: %s <file.asm>\n", argv[0]);
        exit(1);
    }
    asmName = argv[optind];

    // Enforce that the input file name must end in ".asm".  It makes the file
    // name computation a lot easier and it guards against accidentally using
    // file.hex or file.lst as the input file.
    int fileNameLen = strlen(asmName);
    if ((fileNameLen < 5) || strcmp(asmName + fileNameLen - 4, ".asm") != 0)
    {
        fprintf(stderr, "usage: %s <file.asm>\n", argv[0]);
        return -1;
    }

    asmFile = fopen(asmName, "r");
    if (!asmFile)
    {
        fprintf(stderr, "Error opening file:%s\n", asmName);
        return -1;
    }
    listName = strdup(asmName);
    hexName = strdup(asmName);
    strcpy(listName + fileNameLen - 3, "lst");
    strcpy(hexName + fileNameLen - 3, "hex");

    // Open the list file and the hex file.
    if ((listFile = fopen(listName, "w")) == NULL)
    {
        fprintf(stderr, "Error opening file for write: %s\n", listName);
        return -1;
    }
    if ((hexFile = fopen(hexName, "w")) == NULL)
    {
        fprintf(stderr, "Error opening file for write: %s\n", hexName);
        return -1;
    }
    fprintf(listFile, "asm85 8085 Assembler by nib\n\n");

    // Pass 1.  Send all error output to stdout.
    // Compute addresses and store all labels in the symbol table.
    AsmLine asmLine;
    int lineNum = 1;
    int errorCount = 0;
    int warningCount = 0;
    while(fgets(line, 256, asmFile))
    {
        line[strcspn(line, "\r\n")] = '\0';
        int status = asmLine.Process(line, addr, 1);
        if (status == AsmLine::RET_PASS1_ERROR)
        {
            printf("%d: ERROR - %s\n", lineNum, asmLine.GetErrorMsg());
            ++errorCount;
        }
        else if (status == AsmLine::RET_DIR_DS)
        {
            addr = asmLine.GetStartAddr() + asmLine.GetAuxValue();
        }
        else
        {
            addr = asmLine.GetStartAddr() + asmLine.GetNumBytes();
        }
        ++lineNum;
    }
    if (errorCount > 0)
    {
        // The listing isn't generated until the second pass.  If any errors
        // were detected that are specific to pass 1, just stop.
        printf("Errors detected in source.  No hex file created.\n");
        fclose(asmFile);
        fclose(listFile);
        fclose(hexFile);
        return -1;
    }

    // Pass 2.  Send all output to the list file.
    // Assemble the program, using the now complete symbol table, and generate
    // the hex file of the object code.
    rewind(asmFile);
    addr = 0;
    lineNum = 1;
    while (fgets(line, 256, asmFile))
    {
        line[strcspn(line, "\r\n")] = '\0';
        int status = asmLine.Process(line, addr, 2);
        addr = asmLine.GetStartAddr();
        // printf("addr=%04x  label=%s  mnem=%s  line=%s", addr, asmLine.pLabel, asmLine.pMnemonic, line);

        // Store the assembled bytes for this line.
        int byteCount = asmLine.GetNumBytes();
        image.Store(addr, asmLine.GetBytes(), byteCount);

        // Print up to 4 bytes of progam memory on the list line.
        // Note that the additional info printed (addr, bytes, lineNo) is
        // exactly 24 bytes.  This keeps the source code aligned on its 8 byte
        // tab boundaries in the list file.
        if (status == AsmLine::RET_DIR_EQU)
        {
            fprintf(listFile, "(%04x)           ", asmLine.GetAuxValue());
        }
        else if (status == AsmLine::RET_DIR_DS)
        {
            fprintf(listFile, "%04x +%04x       ",
                    addr, asmLine.GetAuxValue());
            addr += asmLine.GetAuxValue();
        }
        else if (byteCount)
        {
            fprintf(listFile, "%04x ", addr);
            for (int ix = 0; ((ix < byteCount) && (ix < 4)); ix++)
            {
                fprintf(listFile, "%02x ", asmLine.GetBytes()[ix]);
            }
            if (byteCount < 4)  fprintf(listFile, "   ");
            if (byteCount < 3)  fprintf(listFile, "   ");
            if (byteCount < 2)  fprintf(listFile, "   ");
        }
        else
        {
            fprintf(listFile, "                 ");
        }

        // Print the line and any error message.
        fprintf(listFile, "%5d: %s\n", lineNum, line);
        switch (status)
        {
        case AsmLine::RET_ERROR:
            printf("%d: ERROR - %s\n", lineNum, asmLine.GetErrorMsg());
            fprintf(listFile, "ERROR - %s\n\n", asmLine.GetErrorMsg());
            ++errorCount;
            break;
        case AsmLine::RET_WARNING:
            printf("%d: WARNING - %s\n", lineNum, asmLine.GetErrorMsg());
            fprintf(listFile, "WARNING - %s\n\n", asmLine.GetErrorMsg());
            ++warningCount;
            break;
        }

        // If the line created more than 4 bytes of program, print the rest
        // on additional listing lines.
        if (byteCount > 4)
        {
            for (int ix = 4; (ix < byteCount); ix++)
            {
                if ((ix & 3) == 0)
                {
                    fprintf(listFile, "     ");
                }
                fprintf(listFile, "%02x%c", asmLine.GetBytes()[ix], ((ix & 3) == 3) ? '\n' : ' ');
            }
            if (((byteCount - 1) & 3) != 3)
            {
                fprintf(listFile, "\n");
            }
        }

        addr += asmLine.GetNumBytes();
        ++lineNum;
    }
    fclose(asmFile);

    fprintf(listFile, "\n%d lines, %d errors, %d warnings\n", lineNum - 1, errorCount, warningCount);
    fprintf(listFile, "\n\nSYMBOL TABLE:\n\n");
    symbols.Dump(listFile);
    fprintf(listFile, "\n\nTotal memory is %d bytes\n", image.GetNumEntries());
    fclose(listFile);

    image.WriteHexFile(hexFile);
    fclose(hexFile);

    if (numBins > 0)
    {
        int aLen = strlen(asmName);
        binName = (char *) malloc(aLen + 6);

        for (int ix = 0; (ix < numBins); ix++)
        {
            // From "file.asm" and address "1234:5678", make "file-1234.bin".
            strcpy(binName, asmName);
            binName[aLen - 4] = '-';
            binAddrs[ix][4] = '\0';
            strcpy(binName + aLen - 3, binAddrs[ix]);
            strcpy(binName + aLen + 1, ".bin");
            if ((binFile = fopen(binName, "wb")) == NULL)
            {
                fprintf(stderr, "Error opening file for write: %s\n", binName);
                return -1;
            }
            unsigned start = (unsigned) strtoul(binAddrs[ix], NULL, 16);
            unsigned end = (unsigned) strtoul(binAddrs[ix] + 5, NULL, 16);
            image.WriteBinFile(binFile, start, end);
            fclose(binFile);
        }
        free(binName);
    }

    return 0;
}
