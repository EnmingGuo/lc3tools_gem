#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include<string.h>

/* unix only */
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>


unsigned counter =0;
/*
 * Input Buffering Windows
 */
struct termios original_tio;

void disable_input_buffering()
{
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

uint16_t check_key()
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

/*
 * Handle Interrupt
 */
void handle_interrupt(int signal)
{
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

/*
 * Trap Code
 */
enum {
    TRAP_GETC = 0x20,  /* get character from keyboard, not echoed onto the terminal */
    TRAP_OUT = 0x21,   /* output a character */
    TRAP_PUTS = 0x22,  /* output a word string */
    TRAP_IN = 0x23,    /* get character from keyboard, echoed onto the terminal */
    TRAP_PUTSP = 0x24, /* output a byte string */
    TRAP_HALT = 0x25,   /* halt the program */
};


/*
 * Store the Memory in an array
 */
#define MEMORY_MAX (1 << 16)
uint16_t memory[MEMORY_MAX];  /* 65536 locations */

enum
{
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC, /* program counter */
    R_COND, /* conditional flag */
    R_COUNT
};

/*
 * Store the Register in an array
 */
uint16_t reg[R_COUNT];

/*
 * opcode
 */
enum
{
    OP_BR = 0, /* branch */
    OP_ADD,    /* add  */
    OP_LD,     /* load */
    OP_ST,     /* store */
    OP_JSR,    /* jump register */
    OP_AND,    /* bitwise and */
    OP_LDR,    /* load register */
    OP_STR,    /* store register */
    OP_RTI,    /* unused */
    OP_NOT,    /* bitwise not */
    OP_LDI,    /* load indirect */
    OP_STI,    /* store indirect */
    OP_JMP,    /* jump */
    OP_RES,    /* reserved (unused) */
    OP_LEA,    /* load effective address */
    OP_TRAP    /* execute trap */
};

/*
 * conditional flag: << 左移符号，左移k位，相当于2^k倍
 */
enum
{
    FL_POS = 1 << 0, /* P */
    FL_ZRO = 1 << 1, /* Z */
    FL_NEG = 1 << 2, /* N */
};


/*
 * Memory Mapped Registers
 */
enum
{
    MR_KBSR = 0xFE00, /* keyboard status */
    MR_KBDR = 0xFE02  /* keyboard data */
};
/*-------------------------以上是所有的硬件组件----------------------------------*/


/*--------------------------实现指令所需要的常用函数------------------------------*/
/*
 * 符号扩展
 */
uint16_t sign_extend(uint16_t x, int bit_count)
{
    /* x是要被扩展的数, bit_count是当前位数*/
    if ((x >> (bit_count - 1)) & 1) { /* 只保留符号位，如果符号位是1，则扩展成1 */
        x |= (0xFFFF << bit_count);
    }
    return x;
}
/*
 * 每当一个值被写入寄存器时，我们都需要更新标志以指示其符号。
 */
void update_flags(uint16_t r)
{
    if (reg[r] == 0)
    {
        reg[R_COND] = FL_ZRO;
    }
    else if (reg[r] >> 15) /* 右移15位留下首位, a 1 in the left-most bit indicates negative */
    {
        reg[R_COND] = FL_NEG;
    }
    else
    {
        reg[R_COND] = FL_POS;
    }
}

/*
 * swap
 */
uint16_t swap16(uint16_t x)
{
    return (x << 8) | (x >> 8);
}

/*
 * 指令是如何进入内存
 */
void read_image_file(FILE* file)
{
    /* the origin tells us where in memory to place the image */
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin); /* 前16位起始地址*/

    /* we know the maximum file size so we only need one fread */
    uint16_t max_read = MEMORY_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    /* swap to little endian */
    while (read-- > 0)
    {
        *p = swap16(*p);
        ++p;
    }
}

/*
 * 更方便的read images
 */
int read_image(const char* image_path)
{
    FILE* file = fopen(image_path, "rb");
    if (!file) { return 0; };
    read_image_file(file);
    fclose(file);
    return 1;
}

/*
 * 内存映射寄存器使得内存访问更加复杂。
 * 我们不能直接读写内存数组，而是必须调用setter和getter函数。
 * 当从KBSR读取内存时，getter将检查键盘并更新两个内存位置。
 */
void mem_write(uint16_t address, uint16_t val)
{
    memory[address] = val;
}

uint16_t mem_read(uint16_t address)
{
    if (address == MR_KBSR)
    {
        if (check_key())
        {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        }
        else
        {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[address];
}
void dump_memory(){
    counter++;
    FILE *fp = NULL;
    //string fileName="memory_dump_p2_"+counter+".txt"
    switch(counter){
        case 1:
            fp = fopen("memory_dump_p2_1.txt", "w");
            break;
        case 2:
            fp = fopen("memory_dump_p2_2.txt", "w");
            break;
        case 3:
            fp = fopen("memory_dump_p2_3.txt", "w");
            break;
        case 4:
            fp = fopen("memory_dump_p2_4.txt", "w");
            break;
        case 5:
            fp = fopen("memory_dump_p2_5.txt", "w");
            break;
        case 6:
            fp = fopen("memory_dump_p2_6.txt", "w");
            break;
        case 7:
            fp = fopen("memory_dump_p2_7.txt", "w");
            break;
        case 8:
            fp = fopen("memory_dump_p2_8.txt", "w");
            break;
        case 9:
            fp = fopen("memory_dump_p2_9.txt", "w");
            break;
        case 10:
            fp = fopen("memory_dump_p2_10.txt", "w");
            break;
        case 11:
            fp = fopen("memory_dump_p2_11.txt", "w");
            break;
        case 12:
            fp = fopen("memory_dump_p2_12.txt", "w");
            break;
    }
    //fp = fopen(fileName.c_str(), "w");
    
    for(int i=0;i<MEMORY_MAX;i++){
        if(memory[i]!=0&& i>=512 && i<=516){
            fprintf(fp,"M%d: %d\n",i,memory[i]);
        }
        //fprintf(fp,"M%d: %d\n",i,memory[i]);
        
    }
   
    for(int i=0;i<R_COUNT;i++){
        if(reg[i]){
            fprintf(fp,"R%d: %d\n",i,reg[i]);
        }
        //fprintf(fp,"R%d: %d\n",i,reg[i]);
    }
    //fprintf(fp, "This is testing for fprintf...\n");
    //fputs("This is testing for fputs...\n", fp);
    fclose(fp);
}
void dump_Q21(){
    FILE *fp=NULL;
    fp=fopen("memory_dump_p1_1.txt","w");
    for(int i=0;i<MEMORY_MAX;i++){
        fprintf(fp,"M%d: %d\n",i,memory[i]);
    }
    for(int i=0;i<R_COUNT;i++){
        fprintf(fp,"R%d: %d\n",i,reg[i]);
    }
    fclose(fp);
}

int main(int argc, const char* argv[]){
    /*
     * Load Arguments: 当我们在主循环时，让我们处理命令行输入，使我们的程序可用。
     * 我们期望有一个或多个VM映像路径，如果没有给出，则给出一个usage string。
     */
    if (argc < 2) /* 参数（VM映射路径）少于1*/
    {
        /* show usage string */
        printf("lc3 [image-file1] ...\n");
        exit(2);
    }

    for (int j = 1; j < argc; ++j)
    {
        if (!read_image(argv[j]))
        {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }
    /*
     * Setup
     */
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    /* since exactly one condition flag should be set at any given time, set the Z flag */
    reg[R_COND] = FL_ZRO;

    /* set the PC to starting position，PC默认的起始位置是0x3000 */
    /* 0x3000 is the default */
    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running = 1;
    while (running){ /* 在主循环中框架是指令执行过程 */
        /* FETCH */
        //printf("PC: %d  ",reg[R_PC]);
        uint16_t instr = mem_read(reg[R_PC]++); /* step1&2: 从memory中取指令,寄存器pc增长 */
        uint16_t op = instr >> 12; /* 4位opcode，所以右移12位得到opcode*/
        //printf("op: %d \n",op); /*测试一下之后进入了什么鬼东西*/
        

        switch (op) { /* step3:查看opcode是那种任务类型 */
            /* step4: 使用指令中的参数argv[]执行指令*/
            case OP_ADD:
                {
                    /* destination register (DR) */
                    uint16_t r0 = (instr >> 9) & 0x7; /* 右移9位，并把前四位消掉，得到DR*/
                    /* first operand (SR1) */
                    uint16_t r1 = (instr >> 6) & 0x7;/* 右移9位，并把前四位消掉，得到第一位寄存器 */
                    /* whether we are in immediate mode */ /* 第5位1: 立即数模式*/
                    uint16_t imm_flag = (instr >> 5) & 0x1;

                    if (imm_flag)/* 是立即数模式 */
                    {
                        uint16_t imm5 = sign_extend(instr & 0x1F, 5); /* 符号扩展，取立即数*/
                        reg[r0] = reg[r1] + imm5; /* ADD */
                        //printf("ADD（立即数）:%d\n",reg[r0]);
                    }
                    else /* 非立即数模式 */
                    {
                        uint16_t r2 = instr & 0x7; /* 取第二个寄存器的值 */
                        reg[r0] = reg[r1] + reg[r2];
                        //printf("ADD（非立即数）:%d\n",reg[r0]);
                    }

                    update_flags(r0); /* 更新conditional flag来标示符号 */
                }
                break;
            case OP_AND:   /*加法操作*/
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;
                    uint16_t imm_flag = (instr >> 5) & 0x1;

                    if (imm_flag)
                    {
                        uint16_t imm5 = sign_extend(instr & 0x1F, 5);
                        reg[r0] = reg[r1] & imm5;
                    }
                    else
                    {
                        uint16_t r2 = instr & 0x7;
                        reg[r0] = reg[r1] & reg[r2];
                    }
                    update_flags(r0);
                }
                break;
            case OP_NOT:  /*反操作*/
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;

                    reg[r0] = ~reg[r1];
                    update_flags(r0);
                }
                break;
            case OP_BR:
                {
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    uint16_t cond_flag = (instr >> 9) & 0x7;
                    if (cond_flag & reg[R_COND])
                    {
                        reg[R_PC] += pc_offset;
                    }
                }
                break;
            case OP_JMP:
                {
                    /* Also handles RET */
                    uint16_t r1 = (instr >> 6) & 0x7;
                    reg[R_PC] = reg[r1];
                    //printf("JMP:%d\n",reg[R_PC]);
                    
                }
                break;
            case OP_JSR:
                {
                    uint16_t long_flag = (instr >> 11) & 1;
                    reg[R_R7] = reg[R_PC];
                    if (long_flag)
                    {
                        uint16_t long_pc_offset = sign_extend(instr & 0x7FF, 11);
                        reg[R_PC] += long_pc_offset;  /* JSR */
                    }
                    else
                    {
                        uint16_t r1 = (instr >> 6) & 0x7;
                        reg[R_PC] = reg[r1]; /* JSRR */
                    }
                }
                break;
            case OP_LD:
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    reg[r0] = mem_read(reg[R_PC] + pc_offset);
                    //printf("LD:destination register: %d\n",reg[r0]);
                    update_flags(r0);
                }
                break;
            case OP_LDI:
                {
                    /* destination register (DR) */
                    uint16_t r0 = (instr >> 9) & 0x7;
                    /* PCoffset 9*/
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    /* add pc_offset to the current PC, look at that memory location to get the final address */
                    reg[r0] = mem_read(mem_read(reg[R_PC] + pc_offset));
                    //printf("LDI:destination register: %d\n",reg[r0]);
                    update_flags(r0);
                }
                break;
            case OP_LDR:
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;
                    uint16_t offset = sign_extend(instr & 0x3F, 6);
                    reg[r0] = mem_read(reg[r1] + offset);
                    //printf("LDR:destination register: %d\n",reg[r0]);
                    update_flags(r0);
                }
                break;
            case OP_LEA:
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    reg[r0] = reg[R_PC] + pc_offset;
                    update_flags(r0);
                }
                break;
            case OP_ST:
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    mem_write(reg[R_PC] + pc_offset, reg[r0]);
                }
                break;
            case OP_STI:
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t pc_offset = sign_extend(instr & 0x1FF, 9);
                    mem_write(mem_read(reg[R_PC] + pc_offset), reg[r0]);
                }
                break;
            case OP_STR:
                {
                    uint16_t r0 = (instr >> 9) & 0x7;
                    uint16_t r1 = (instr >> 6) & 0x7;
                    uint16_t offset = sign_extend(instr & 0x3F, 6);
                    mem_write(reg[r1] + offset, reg[r0]);
                }
                break;
            case OP_TRAP:
                {
                    reg[R_R7] = reg[R_PC];
                    switch (instr & 0xFF) {
                        case TRAP_GETC:
                            /* read a single ASCII char */
//                            reg[R_R0] = (uint16_t)getchar();
//                            update_flags(R_R0);
                            break;
                        case TRAP_OUT:
//                            putc((char)reg[R_R0], stdout);
//                            fflush(stdout);
                            break;
                        case TRAP_PUTS:
//                            {
//                                /* one char per word */
//                                uint16_t* c = memory + reg[R_R0];
//                                while (*c)
//                                {
//                                    putc((char)*c, stdout);
//                                    ++c;
//                                }
//                                fflush(stdout);
//                            }
                            break;
                        case TRAP_IN:
//                            {
//                                printf("Enter a character: ");
//                                char c = getchar();
//                                putc(c, stdout);
//                                fflush(stdout);
//                                reg[R_R0] = (uint16_t)c;
//                                update_flags(R_R0);
//                            }
                           break;
                        case TRAP_PUTSP:
//                            {
//                                /* one char per byte (two bytes per word)
//                                   here we need to swap back to
//                                   big endian format */
//                                uint16_t* c = memory + reg[R_R0];
//                                while (*c)
//                                {
//                                    char char1 = (*c) & 0xFF;
//                                    putc(char1, stdout);
//                                    char char2 = (*c) >> 8;
//                                    if (char2) putc(char2, stdout);
//                                    ++c;
//                                }
//                                fflush(stdout);
//                            }
                            break;
                        case TRAP_HALT:
//                            puts("HALT");
//                            fflush(stdout);
//                            running = 0;
                             reg[R_PC]=memory[TRAP_HALT];
                             break;
                        default:
                            puts("DUMP");
                            //dump_Q21();
                            dump_memory();
                            break;
                    }
                }
                break;
            case OP_RES:
            case OP_RTI:
            default:
                abort();
                break;
        }
    }
    restore_input_buffering();
}
