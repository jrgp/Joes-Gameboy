#include <SDL.h>

#include<unistd.h>
#include<inttypes.h>
#include<stdio.h>
#include<stdlib.h>
#include<stdbool.h>
#include<string.h>
#include"bios.h"
#include"bits.h"
#include"constants.h"
#include"opnames.h"

typedef uint8_t byte;
typedef uint16_t word;

//
// GPU
//
int gpu_cycles;
byte VRAM[0xffff + 1];

SDL_Surface* surface;
uint32_t *pixels;

typedef struct {
    bool enabled, bg, window, sprite;
    int windowtilemap, BgWindowTileData;
    bool BgTileDataSigned;
    int bgtilemap;
} GPUCONTROL;

GPUCONTROL gpu_control;

void gpu_parse_control(byte control){
    gpu_control.enabled = bit_check(control, 7);
    gpu_control.window = bit_check(control, 5);
    gpu_control.sprite = bit_check(control, 1);
    gpu_control.bg = bit_check(control, 0);

    if (bit_check(control, 4)) {
        gpu_control.BgWindowTileData = 0x8000;
        gpu_control.BgTileDataSigned = false;
    } else {
        gpu_control.BgWindowTileData = 0x8800;
        gpu_control.BgTileDataSigned = true;
    }

    if(bit_check(control, 3)){
      gpu_control.bgtilemap = 0x9C00;
    } else {
      gpu_control.bgtilemap = 0x9800;
    }
}

#define set_pixel(x,y,c) pixels[(y * surface->w) + x] = c;

void gpu_init(){
    memset(VRAM, 0, 0xffff + 1);
    gpu_cycles = 0;
}

byte gpu_read(int pos){
    printf("reading from gpu %x\n", pos);
    return VRAM[pos];
}

void gpu_write(int pos, byte data){
    VRAM[pos] = data;
    if (pos == LCDC) {
        gpu_parse_control(data);
    } else if (data > 0) {
 //       printf("wrote %x to %x\n", data, pos);
    }
}

uint32_t gpu_pallete_color(byte number, int paletteIndex) {
    byte config = VRAM[paletteIndex];
    byte resultindex = 0;

    byte b1 = number * 2;
    byte b2 = (number * 2) + 1;

    if(bit_check(config, b1)){
        resultindex = bit_set(resultindex, 1);
    }

    if(bit_check(config, b2)){
        resultindex = bit_set(resultindex, 0);
    }

    uint32_t color = pallette[resultindex];

    return color;
}

void gpu_draw_bg(byte ly){
    if (gpu_control.bg) {
        for (int tile = 0; tile < 32; tile++) {
            // Index of tile from sprite map
            int tileIndex = VRAM[gpu_control.bgtilemap + ((ly / 8)*32) + tile];
            // Start of tile data
            int start = gpu_control.BgWindowTileData + (16 * tileIndex);

            byte high = VRAM[start + ((ly % 8)*2)];
            byte low = VRAM[start + ((ly % 8)*2)+1];
            byte x = 7;
            for (byte i = 0; i < 8; i++) {
              byte wat = 0;
              if(bit_check(low, i)){
                wat |= (1 << 1);
              }
              if(bit_check(high, i)){
                wat |= (1 << 0);
              }

              set_pixel((tile*8)+x, ly, gpu_pallete_color(wat, BGP));
              x--;
            }
        }

    } else {
        for (int i = 0; i<WIDTH; i++) {
            set_pixel(i, ly, 0xffffff);
        }
    }
}

void gpu_drawline(byte ly){
    gpu_draw_bg(ly);
}

void gpu_step(int _cycles){
    if (gpu_control.enabled) {
        gpu_cycles += _cycles;
        if (gpu_cycles >= 456) {
            gpu_cycles = 0;

            VRAM[LY]++;
            byte ly = VRAM[LY];

            if (ly == 144) {

            } else if (ly > 153) {
                VRAM[LY] = 0;
            } else if (ly < 144) {
                // draw scanline
                gpu_drawline(ly);
            }
        }
    }
}

//
// CART
//

char cart_name[30];
byte *cart_data;
byte cart_read(int pos){
  // TODO: banking obviously
  return cart_data[pos];
}

void cart_write(int pos, byte data){
  printf("unhandled cart write to %x\n", pos);
}

void cart_load(char *path) {
    FILE *fileptr;
    long filelen;
    fileptr = fopen(path, "rb");
    fseek(fileptr, 0, SEEK_END);
    filelen = ftell(fileptr);
    rewind(fileptr);
    cart_data = (byte *)malloc(filelen * sizeof(byte));
    if (cart_data == NULL) {
        perror("malloc");
        exit(1);
    }
    fread(cart_data, filelen, 1, fileptr);
    fclose(fileptr);

    int i = 0;

    for (int pos = 0x0134; pos <= 0x0144; pos++, i++) {
        byte c = cart_data[pos];
        if (c == 0 || (char) c == ' ') {
            break;
        }
        cart_name[i] = c;
    }
    cart_name[i+1] = '\0';

    printf("Loaded %s\n", cart_name);
}

//
// RAM
//
byte RAM[0xffff + 1];
bool inBios;
bool bailAfterBios;

byte mem_read(int pos) {
    if (pos < 256 && inBios) {
        return bios[pos];
    } else if (pos <= 0x7FFF) {
        return cart_read(pos);
    } else if ((pos >= 0xA000 && pos <= 0xBFFF) || (pos >= 0x8000 && pos <= 0x9FFF)) {
        return gpu_read(pos);
    }
    // FIXME: lots missing here
    else {
        switch (pos) {
            case BGP:
            case LCDC:
            case LY:
            case LYC:
            case SCY:
            case WX:
            case WY:
                return gpu_read(pos);
            default:
                return RAM[pos];
        }
    }
}

void mem_write(int pos, byte data) {
    switch (pos) {
        case BGP:
        case LCDC:
        case LY:
        case LYC:
        case SCY:
        case WX:
        case WY:
            gpu_write(pos, data);
        default:
            if (pos == 0xFF50 && inBios) {
                inBios = false;
                printf("bios disabled\n");
            } else if ((pos >= 0xA000 && pos <= 0xBFFF) || (pos >= 0x8000 && pos <= 0x9FFF)) {
                gpu_write(pos, data);
            } else {
                RAM[pos] = data;
            }
    }
}

void mem_init(){
    memset(RAM, 0, 0xffff + 1);
    inBios = true;
    bailAfterBios = true;
    bailAfterBios = false;
}

//
// CPU
//

#define FLAG_Z 7 // Zero
#define FLAG_N 6 // Subtraction
#define FLAG_H 5 // Half carry
#define FLAG_C 4 // Cary

byte F, A, C, B, E, D, L, H;

int cycles;
int PC;
word SP;
bool interrupts;

void cpu_init(){
    F = 0;
    A = 0;
    C = 0;
    B = 0;
    E = 0;
    D = 0;
    L = 0;
    H = 0;
    cycles = 0;
    PC = 0;
    SP = 0;
}

void push_stack(word data) {
    byte f1 = data & 0x00ff;
    byte f2 = (data & 0xff00) >> 8;

    SP -= 2;
    mem_write(SP, f1);
    mem_write(SP + 1, f2);
}

word pop_stack() {
    word f1 = mem_read(SP);
    word f2 = mem_read(SP + 1);
    SP += 2;
    return (f2 << 8) | f1;
}

word peek_stack() {
    word f1 = mem_read(SP);
    word f2 = mem_read(SP + 1);
    return (f2 << 8) | f1;
}

word AF() {
    return F | (A << 8);
}

word HL() {
    return L | (H << 8);
}

word DE() {
    return E | (D << 8);
}

word BC() {
    return C | (B << 8);
}


void setAF(word data) {
    F = (byte) ((byte) (data & 0x00ff) & 0xf0);
    A = (byte) ((byte) data & 0xff00 >> 8);
}

void setBC(word data) {
    B = (byte) (data >> 8);
    C = (byte) data;
}

void setDE(word data) {
    D = (byte) (data >> 8);
    E = (byte) (data);
}

void setHL(word data) {
    H = (byte) (data >> 8);
    L = (byte) data;
}

word HLDec() {
    word hl = HL();
    setHL(hl - 1);
    // System.out.println("Old HL: 0x"+Integer.toHexString(hl));
    return hl;
}

word HLInc() {
    word hl = HL();
    setHL(hl + 1);
    return hl;
}

byte cpu_read(int loc) {
  return mem_read(loc);
}

void cpu_write(int loc, byte value) {
    cycles += 4;
    mem_write(loc, value);
}

byte cpu_read_next() {
  byte data = cpu_read(PC);
  PC++;
  return data;
}

word cpu_read16() {
  byte lo = cpu_read_next();
  byte hi = cpu_read_next();
  return lo | (hi << 8);
}


//
// ALGS
//
void setFlag(byte flag, bool value) {
    F = value ? bit_set(F, flag) : bit_clear(F, flag);
}

bool CheckFlag(byte flag) {
    return bit_check(F, flag);
}

void clearFlags() {
    F = 0;
}

byte Inc(byte reg) {
    reg++;
    setFlag(FLAG_Z, reg == 0);
    // FIXME: flags + signing + etc
    return reg;
}

byte Dec(byte reg) {
    reg--;
    setFlag(FLAG_Z, reg == 0);
    setFlag(FLAG_N, true);
    // FIXME: flags + signing + etc
    return reg;
}

byte Sub(byte arg) {
    int result = A - arg;
    setFlag(FLAG_Z, (byte) result == 0);
    setFlag(FLAG_N, true);
    // FIXME: flags + signing + etc
    return (byte) result;
}

byte Add(byte arg) {
    int result = A + arg;
    setFlag(FLAG_Z, (byte) result == 0);
    // FIXME: flags + signing + etc
    return (byte) result;
}

byte And(byte arg) {
    byte v = A & arg;
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    setFlag(FLAG_H, true);
    return v;
}

byte Or(byte arg) {
    byte v = A | arg;
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte Xor(byte arg) {
    byte v = A ^ arg;
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte Swap(byte arg) {
    byte v = ((arg & 0x0F)<<4 | (arg & 0xF0)>>4);
    clearFlags();
    setFlag(FLAG_Z, v == 0);
    return v;
}

byte RL(byte arg) {
    bool oldC = (arg & 0x80) == 0x80;
    arg <<= 1;
    if (CheckFlag(FLAG_C)) {
        arg ^= 1;
    }
    clearFlags();
    setFlag(FLAG_C, oldC);
    setFlag(FLAG_Z, arg == 0); // For RLA this is later set to false.
    // FIXME: this could be very wrong
    return arg;
}

void Bit(byte target, int bit) {
    setFlag(FLAG_Z, !bit_check(target, (byte) bit));
    setFlag(FLAG_N, false);
    setFlag(FLAG_H, true);
}

void CP(byte to) {
    int result = A - to;
    setFlag(FLAG_N, true);
    setFlag(FLAG_C, result < 0);
    setFlag(FLAG_Z, result == 0);
    // FIXME: carry flag
}

//
// CPU helpers
//

void exec_ext_op(byte opcode);

void exec_op(byte opcode){
    byte offset;
    int pos;
    switch (opcode){
        // NOP
        case 0:
            break;

        // Extended opcodes
        case 0xcb:
            exec_ext_op(cpu_read_next());
            break;

// START GENERATED

    // LD BC <- d16
    case 0x01:
      setBC(cpu_read16());
      break;

    // LD (BC) <- A
    case 0x02:
      cpu_write(BC(), A);
      break;

    // INC BC
    case 0x03:
      setBC(BC() + 1);
      cycles += 4;
      break;

    // INC B
    case 0x04:
      B = Inc(B);
      break;

    // DEC B
    case 0x05:
      B = Dec(B);
      break;

    // LD B <- d8
    case 0x06:
      B = cpu_read_next();
      break;

    // ADD HL += BC
    case 0x09:
        {
            word target = HL();
            word source = BC();
            word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x07FF)+(source&0x07FF) > 0x07FF);
        }
      break;

    // LD A <- (BC)
    case 0x0a:
      A = cpu_read(BC());
      break;

    // DEC BC
    case 0x0b:
      setBC(BC() - 1);
      cycles += 4;
      break;

    // INC C
    case 0x0c:
      C = Inc(C);
      break;

    // DEC C
    case 0x0d:
      C = Dec(C);
      break;

    // LD C <- d8
    case 0x0e:
      C = cpu_read_next();
      break;

    // LD DE <- d16
    case 0x11:
      setDE(cpu_read16());
      break;

    // LD (DE) <- A
    case 0x12:
      cpu_write(DE(), A);
      break;

    // INC DE
    case 0x13:
      setDE(DE() + 1);
      cycles += 4;
      break;

    // INC D
    case 0x14:
      D = Inc(D);
      break;

    // DEC D
    case 0x15:
      D = Dec(D);
      break;

    // LD D <- d8
    case 0x16:
      D = cpu_read_next();
      break;

    // RLA
    case 0x17:
      A = RL(A);
      setFlag(FLAG_Z, false);
      break;

    // JR r8
    case 0x18:
      offset = cpu_read_next();
      PC += (int8_t) offset;
      cycles += 4;
      break;

    // ADD HL += DE
    case 0x19:
        {
            word target = HL();
            word source = DE();
            word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x07FF)+(source&0x07FF) > 0x07FF);
        }
      break;

    // LD A <- (DE)
    case 0x1a:
      A = cpu_read(DE());
      break;

    // DEC DE
    case 0x1b:
      setDE(DE() - 1);
      cycles += 4;
      break;

    // INC E
    case 0x1c:
      E = Inc(E);
      break;

    // DEC E
    case 0x1d:
      E = Dec(E);
      break;

    // LD E <- d8
    case 0x1e:
      E = cpu_read_next();
      break;

    // JR NZ
    case 0x20:
      offset = cpu_read_next();
      if (!CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // LD HL <- d16
    case 0x21:
      setHL(cpu_read16());
      break;

    // LD (HL+) <- A
    case 0x22:
      cpu_write(HLInc(), A);
      break;

    // INC HL
    case 0x23:
      setHL(HL() + 1);
      cycles += 4;
      break;

    // INC H
    case 0x24:
      H = Inc(H);
      break;

    // DEC H
    case 0x25:
      H = Dec(H);
      break;

    // LD H <- d8
    case 0x26:
      H = cpu_read_next();
      break;

    // JR Z
    case 0x28:
      offset = cpu_read_next();
      if (CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // ADD HL += HL
    case 0x29:
        {
            word target = HL();
            word source = HL();
            word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x07FF)+(source&0x07FF) > 0x07FF);
        }
      break;

    // LD A <- (HL+)
    case 0x2a:
      A = cpu_read(HLInc());
      break;

    // DEC HL
    case 0x2b:
      setHL(HL() - 1);
      cycles += 4;
      break;

    // INC L
    case 0x2c:
      L = Inc(L);
      break;

    // DEC L
    case 0x2d:
      L = Dec(L);
      break;

    // LD L <- d8
    case 0x2e:
      L = cpu_read_next();
      break;

    // CPL
    case 0x2f:
      A = ~A;
      break;

    // JR NC
    case 0x30:
      offset = cpu_read_next();
      if (!CheckFlag(FLAG_C)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // LD SP <- d16
    case 0x31:
      SP = cpu_read16();
      break;

    // LD (HL-) <- A
    case 0x32:
      cpu_write(HLDec(), A);
      break;

    // INC SP
    case 0x33:
      SP++;
      cycles += 4;
      break;

    // LD (HL) <- d8
    case 0x36:
      cpu_write(HL(), cpu_read_next());
      break;

    // JR C
    case 0x38:
      offset = cpu_read_next();
      if (CheckFlag(FLAG_C)) {
          cycles += 4;
          PC += (int8_t) offset;
      }
      break;

    // ADD HL += SP
    case 0x39:
        {
            word target = HL();
            word source = SP;
            word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x07FF)+(source&0x07FF) > 0x07FF);
        }
      break;

    // LD A <- (HL-)
    case 0x3a:
      A = cpu_read(HLDec());
      break;

    // DEC SP
    case 0x3b:
      SP--;
      cycles += 4;
      break;

    // INC A
    case 0x3c:
      A = Inc(A);
      break;

    // DEC A
    case 0x3d:
      A = Dec(A);
      break;

    // LD A <- d8
    case 0x3e:
      A = cpu_read_next();
      break;

    // LD B <- B
    case 0x40:
      break;

    // LD B <- C
    case 0x41:
      B = C;
      break;

    // LD B <- D
    case 0x42:
      B = D;
      break;

    // LD B <- E
    case 0x43:
      B = E;
      break;

    // LD B <- H
    case 0x44:
      B = H;
      break;

    // LD B <- L
    case 0x45:
      B = L;
      break;

    // LD B <- (HL)
    case 0x46:
      B = cpu_read(HL());
      break;

    // LD B <- A
    case 0x47:
      B = A;
      break;

    // LD C <- B
    case 0x48:
      C = B;
      break;

    // LD C <- C
    case 0x49:
      break;

    // LD C <- D
    case 0x4a:
      C = D;
      break;

    // LD C <- E
    case 0x4b:
      C = E;
      break;

    // LD C <- H
    case 0x4c:
      C = H;
      break;

    // LD C <- L
    case 0x4d:
      C = L;
      break;

    // LD C <- (HL)
    case 0x4e:
      C = cpu_read(HL());
      break;

    // LD C <- A
    case 0x4f:
      C = A;
      break;

    // LD D <- B
    case 0x50:
      D = B;
      break;

    // LD D <- C
    case 0x51:
      D = C;
      break;

    // LD D <- D
    case 0x52:
      break;

    // LD D <- E
    case 0x53:
      D = E;
      break;

    // LD D <- H
    case 0x54:
      D = H;
      break;

    // LD D <- L
    case 0x55:
      D = L;
      break;

    // LD D <- (HL)
    case 0x56:
      D = cpu_read(HL());
      break;

    // LD D <- A
    case 0x57:
      D = A;
      break;

    // LD E <- B
    case 0x58:
      E = B;
      break;

    // LD E <- C
    case 0x59:
      E = C;
      break;

    // LD E <- D
    case 0x5a:
      E = D;
      break;

    // LD E <- E
    case 0x5b:
      break;

    // LD E <- H
    case 0x5c:
      E = H;
      break;

    // LD E <- L
    case 0x5d:
      E = L;
      break;

    // LD E <- (HL)
    case 0x5e:
      E = cpu_read(HL());
      break;

    // LD E <- A
    case 0x5f:
      E = A;
      break;

    // LD H <- B
    case 0x60:
      H = B;
      break;

    // LD H <- C
    case 0x61:
      H = C;
      break;

    // LD H <- D
    case 0x62:
      H = D;
      break;

    // LD H <- E
    case 0x63:
      H = E;
      break;

    // LD H <- H
    case 0x64:
      break;

    // LD H <- L
    case 0x65:
      H = L;
      break;

    // LD H <- (HL)
    case 0x66:
      H = cpu_read(HL());
      break;

    // LD H <- A
    case 0x67:
      H = A;
      break;

    // LD L <- B
    case 0x68:
      L = B;
      break;

    // LD L <- C
    case 0x69:
      L = C;
      break;

    // LD L <- D
    case 0x6a:
      L = D;
      break;

    // LD L <- E
    case 0x6b:
      L = E;
      break;

    // LD L <- H
    case 0x6c:
      L = H;
      break;

    // LD L <- L
    case 0x6d:
      break;

    // LD L <- (HL)
    case 0x6e:
      L = cpu_read(HL());
      break;

    // LD L <- A
    case 0x6f:
      L = A;
      break;

    // LD (HL) <- B
    case 0x70:
      cpu_write(HL(), B);
      break;

    // LD (HL) <- C
    case 0x71:
      cpu_write(HL(), C);
      break;

    // LD (HL) <- D
    case 0x72:
      cpu_write(HL(), D);
      break;

    // LD (HL) <- E
    case 0x73:
      cpu_write(HL(), E);
      break;

    // LD (HL) <- H
    case 0x74:
      cpu_write(HL(), H);
      break;

    // LD (HL) <- L
    case 0x75:
      cpu_write(HL(), L);
      break;

    // LD (HL) <- A
    case 0x77:
      cpu_write(HL(), A);
      break;

    // LD A <- B
    case 0x78:
      A = B;
      break;

    // LD A <- C
    case 0x79:
      A = C;
      break;

    // LD A <- D
    case 0x7a:
      A = D;
      break;

    // LD A <- E
    case 0x7b:
      A = E;
      break;

    // LD A <- H
    case 0x7c:
      A = H;
      break;

    // LD A <- L
    case 0x7d:
      A = L;
      break;

    // LD A <- (HL)
    case 0x7e:
      A = cpu_read(HL());
      break;

    // LD A <- A
    case 0x7f:
      break;

    // ADD A += B
    case 0x80:
      A = Add(B);
      break;

    // ADD A += C
    case 0x81:
      A = Add(C);
      break;

    // ADD A += D
    case 0x82:
      A = Add(D);
      break;

    // ADD A += E
    case 0x83:
      A = Add(E);
      break;

    // ADD A += H
    case 0x84:
      A = Add(H);
      break;

    // ADD A += L
    case 0x85:
      A = Add(L);
      break;

    // ADD A += (HL)
    case 0x86:
      A = Add(cpu_read(HL()));
      break;

    // ADD A += A
    case 0x87:
      A = Add(A);
      break;

    // SUB A -= B
    case 0x90:
      A = Sub(B);
      break;

    // SUB A -= C
    case 0x91:
      A = Sub(C);
      break;

    // SUB A -= D
    case 0x92:
      A = Sub(D);
      break;

    // SUB A -= E
    case 0x93:
      A = Sub(E);
      break;

    // SUB A -= H
    case 0x94:
      A = Sub(H);
      break;

    // SUB A -= L
    case 0x95:
      A = Sub(L);
      break;

    // SUB A -= (HL)
    case 0x96:
      A = Sub(cpu_read(HL()));
      break;

    // SUB A -= A
    case 0x97:
      A = Sub(A);
      break;

    // AND A & B
    case 0xa0:
      A = And(B);
      break;

    // AND A & C
    case 0xa1:
      A = And(C);
      break;

    // AND A & D
    case 0xa2:
      A = And(D);
      break;

    // AND A & E
    case 0xa3:
      A = And(E);
      break;

    // AND A & H
    case 0xa4:
      A = And(H);
      break;

    // AND A & L
    case 0xa5:
      A = And(L);
      break;

    // AND A & (HL)
    case 0xa6:
      A = And(cpu_read(HL()));
      break;

    // AND A & A
    case 0xa7:
      A = And(A);
      break;

    // XOR A ^ B
    case 0xa8:
      A = Xor(B);
      break;

    // XOR A ^ C
    case 0xa9:
      A = Xor(C);
      break;

    // XOR A ^ D
    case 0xaa:
      A = Xor(D);
      break;

    // XOR A ^ E
    case 0xab:
      A = Xor(E);
      break;

    // XOR A ^ H
    case 0xac:
      A = Xor(H);
      break;

    // XOR A ^ L
    case 0xad:
      A = Xor(L);
      break;

    // XOR A ^ (HL)
    case 0xae:
      A = Xor(cpu_read(HL()));
      break;

    // XOR A ^ A
    case 0xaf:
      A = Xor(A);
      break;

    // OR A | B
    case 0xb0:
      A = Or(B);
      break;

    // OR A | C
    case 0xb1:
      A = Or(C);
      break;

    // OR A | D
    case 0xb2:
      A = Or(D);
      break;

    // OR A | E
    case 0xb3:
      A = Or(E);
      break;

    // OR A | H
    case 0xb4:
      A = Or(H);
      break;

    // OR A | L
    case 0xb5:
      A = Or(L);
      break;

    // OR A | (HL)
    case 0xb6:
      A = Or(cpu_read(HL()));
      break;

    // OR A | A
    case 0xb7:
      A = Or(A);
      break;

    // CP B
    case 0xb8:
      CP(B);
      break;

    // CP C
    case 0xb9:
      CP(C);
      break;

    // CP D
    case 0xba:
      CP(D);
      break;

    // CP E
    case 0xbb:
      CP(E);
      break;

    // CP H
    case 0xbc:
      CP(H);
      break;

    // CP L
    case 0xbd:
      CP(L);
      break;

    // CP (HL)
    case 0xbe:
      CP(cpu_read(HL()));
      break;

    // CP A
    case 0xbf:
      CP(A);
      break;

    // RET NZ
    case 0xc0:
      cycles += 4;
      if (!CheckFlag(FLAG_Z)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // POP BC
    case 0xc1:
      setBC(pop_stack());
      cycles += 8;
      break;

    // JP NZ
    case 0xc2:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // JP a16
    case 0xc3:
      PC = cpu_read16();
      cycles += 4;
      break;

    // CALL NZ
    case 0xc4:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_Z)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // PUSH BC
    case 0xc5:
      push_stack(BC());
      cycles += 12;
      break;

    // RST 00H
    case 0xc7:
      push_stack(PC);
      cycles += 12;
      PC = 0x00;
      break;

    // RET Z
    case 0xc8:
      cycles += 4;
      if (CheckFlag(FLAG_Z)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // RET
    case 0xc9:
      cycles += 12;
      PC = pop_stack();
      break;

    // JP Z
    case 0xca:
      pos = cpu_read16();
      if (CheckFlag(FLAG_Z)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // CALL Z
    case 0xcc:
      pos = cpu_read16();
      if (CheckFlag(FLAG_Z)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // CALL a16
    case 0xcd:
      pos = cpu_read16();
      push_stack(PC);
      PC = pos;
      cycles += 12;
      break;

    // RST 08H
    case 0xcf:
      push_stack(PC);
      cycles += 12;
      PC = 0x08;
      break;

    // RET NC
    case 0xd0:
      cycles += 4;
      if (!CheckFlag(FLAG_C)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // POP DE
    case 0xd1:
      setDE(pop_stack());
      cycles += 8;
      break;

    // JP NC
    case 0xd2:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_C)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // CALL NC
    case 0xd4:
      pos = cpu_read16();
      if (!CheckFlag(FLAG_C)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // PUSH DE
    case 0xd5:
      push_stack(DE());
      cycles += 12;
      break;

    // RST 10H
    case 0xd7:
      push_stack(PC);
      cycles += 12;
      PC = 0x10;
      break;

    // RET C
    case 0xd8:
      cycles += 4;
      if (CheckFlag(FLAG_C)) {
          cycles += 12;
          PC = pop_stack();
      }
      break;

    // JP C
    case 0xda:
      pos = cpu_read16();
      if (CheckFlag(FLAG_C)) {
          cycles += 4;
          PC = pos;
      }
      break;

    // CALL C
    case 0xdc:
      pos = cpu_read16();
      if (CheckFlag(FLAG_C)) {
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }
      break;

    // RST 18H
    case 0xdf:
      push_stack(PC);
      cycles += 12;
      PC = 0x18;
      break;

    // LDH (a8) <- A
    case 0xe0:
      cpu_write(cpu_read_next() + 0xFF00, A);
      break;

    // POP HL
    case 0xe1:
      setHL(pop_stack());
      cycles += 8;
      break;

    // LD (C) <- A
    case 0xe2:
      cpu_write(C + 0xFF00, A);
      break;

    // PUSH HL
    case 0xe5:
      push_stack(HL());
      cycles += 12;
      break;

    // AND A & d8
    case 0xe6:
      A = And(cpu_read_next());
      break;

    // RST 20H
    case 0xe7:
      push_stack(PC);
      cycles += 12;
      PC = 0x20;
      break;

    // JP (HL)
    case 0xe9:
      PC = HL();
      break;

    // LD (a16) <- A
    case 0xea:
      cpu_write(cpu_read16(), A);
      break;

    // XOR A ^ d8
    case 0xee:
      A = Xor(cpu_read_next());
      break;

    // RST 28H
    case 0xef:
      push_stack(PC);
      cycles += 12;
      PC = 0x28;
      break;

    // LDH A <- (a8)
    case 0xf0:
      A = cpu_read(cpu_read_next() + 0xFF00);
      break;

    // POP AF
    case 0xf1:
      setAF(pop_stack());
      cycles += 8;
      break;

    // LD A <- (C)
    case 0xf2:
      A = cpu_read(C + 0xFF00);
      break;

    // DI
    case 0xf3:
      interrupts = false;
      break;

    // PUSH AF
    case 0xf5:
      push_stack(AF());
      cycles += 12;
      break;

    // RST 30H
    case 0xf7:
      push_stack(PC);
      cycles += 12;
      PC = 0x30;
      break;

    // LD A <- (a16)
    case 0xfa:
      A = cpu_read(cpu_read16());
      break;

    // EI
    case 0xfb:
      interrupts = true;
      break;

    // CP d8
    case 0xfe:
      CP(cpu_read_next());
      break;

    // RST 38H
    case 0xff:
      push_stack(PC);
      cycles += 12;
      PC = 0x38;
      break;

// END GENERATED
        default:
            printf("Unimplemented opcode %x at %x\n", opcode, PC - 1);
            exit(1);
            break;
    }
}

void exec_ext_op(byte opcode){
    switch (opcode){
// START EX GENERATED

    // RL B
    case 0x10:
      B = RL(B);
      break;

    // RL C
    case 0x11:
      C = RL(C);
      break;

    // RL D
    case 0x12:
      D = RL(D);
      break;

    // RL E
    case 0x13:
      E = RL(E);
      break;

    // RL H
    case 0x14:
      H = RL(H);
      break;

    // RL L
    case 0x15:
      L = RL(L);
      break;

    // RL (HL)
    case 0x16:
      cpu_write(HL(), RL(cpu_read(HL())));
      break;

    // RL A
    case 0x17:
      A = RL(A);
      break;

    // SWAP B
    case 0x30:
      B = Swap(B);
      break;

    // SWAP C
    case 0x31:
      C = Swap(C);
      break;

    // SWAP D
    case 0x32:
      D = Swap(D);
      break;

    // SWAP E
    case 0x33:
      E = Swap(E);
      break;

    // SWAP H
    case 0x34:
      H = Swap(H);
      break;

    // SWAP L
    case 0x35:
      L = Swap(L);
      break;

    // SWAP (HL)
    case 0x36:
      cpu_write(HL(), Swap(cpu_read(HL())));
      break;

    // SWAP A
    case 0x37:
      A = Swap(A);
      break;

    // BIT 0 of B
    case 0x40:
      Bit(B, 0);
      break;

    // BIT 0 of C
    case 0x41:
      Bit(C, 0);
      break;

    // BIT 0 of D
    case 0x42:
      Bit(D, 0);
      break;

    // BIT 0 of E
    case 0x43:
      Bit(E, 0);
      break;

    // BIT 0 of H
    case 0x44:
      Bit(H, 0);
      break;

    // BIT 0 of L
    case 0x45:
      Bit(L, 0);
      break;

    // BIT 0 of (HL)
    case 0x46:
      Bit(cpu_read(HL()), 0);
      break;

    // BIT 0 of A
    case 0x47:
      Bit(A, 0);
      break;

    // BIT 1 of B
    case 0x48:
      Bit(B, 1);
      break;

    // BIT 1 of C
    case 0x49:
      Bit(C, 1);
      break;

    // BIT 1 of D
    case 0x4a:
      Bit(D, 1);
      break;

    // BIT 1 of E
    case 0x4b:
      Bit(E, 1);
      break;

    // BIT 1 of H
    case 0x4c:
      Bit(H, 1);
      break;

    // BIT 1 of L
    case 0x4d:
      Bit(L, 1);
      break;

    // BIT 1 of (HL)
    case 0x4e:
      Bit(cpu_read(HL()), 1);
      break;

    // BIT 1 of A
    case 0x4f:
      Bit(A, 1);
      break;

    // BIT 2 of B
    case 0x50:
      Bit(B, 2);
      break;

    // BIT 2 of C
    case 0x51:
      Bit(C, 2);
      break;

    // BIT 2 of D
    case 0x52:
      Bit(D, 2);
      break;

    // BIT 2 of E
    case 0x53:
      Bit(E, 2);
      break;

    // BIT 2 of H
    case 0x54:
      Bit(H, 2);
      break;

    // BIT 2 of L
    case 0x55:
      Bit(L, 2);
      break;

    // BIT 2 of (HL)
    case 0x56:
      Bit(cpu_read(HL()), 2);
      break;

    // BIT 2 of A
    case 0x57:
      Bit(A, 2);
      break;

    // BIT 3 of B
    case 0x58:
      Bit(B, 3);
      break;

    // BIT 3 of C
    case 0x59:
      Bit(C, 3);
      break;

    // BIT 3 of D
    case 0x5a:
      Bit(D, 3);
      break;

    // BIT 3 of E
    case 0x5b:
      Bit(E, 3);
      break;

    // BIT 3 of H
    case 0x5c:
      Bit(H, 3);
      break;

    // BIT 3 of L
    case 0x5d:
      Bit(L, 3);
      break;

    // BIT 3 of (HL)
    case 0x5e:
      Bit(cpu_read(HL()), 3);
      break;

    // BIT 3 of A
    case 0x5f:
      Bit(A, 3);
      break;

    // BIT 4 of B
    case 0x60:
      Bit(B, 4);
      break;

    // BIT 4 of C
    case 0x61:
      Bit(C, 4);
      break;

    // BIT 4 of D
    case 0x62:
      Bit(D, 4);
      break;

    // BIT 4 of E
    case 0x63:
      Bit(E, 4);
      break;

    // BIT 4 of H
    case 0x64:
      Bit(H, 4);
      break;

    // BIT 4 of L
    case 0x65:
      Bit(L, 4);
      break;

    // BIT 4 of (HL)
    case 0x66:
      Bit(cpu_read(HL()), 4);
      break;

    // BIT 4 of A
    case 0x67:
      Bit(A, 4);
      break;

    // BIT 5 of B
    case 0x68:
      Bit(B, 5);
      break;

    // BIT 5 of C
    case 0x69:
      Bit(C, 5);
      break;

    // BIT 5 of D
    case 0x6a:
      Bit(D, 5);
      break;

    // BIT 5 of E
    case 0x6b:
      Bit(E, 5);
      break;

    // BIT 5 of H
    case 0x6c:
      Bit(H, 5);
      break;

    // BIT 5 of L
    case 0x6d:
      Bit(L, 5);
      break;

    // BIT 5 of (HL)
    case 0x6e:
      Bit(cpu_read(HL()), 5);
      break;

    // BIT 5 of A
    case 0x6f:
      Bit(A, 5);
      break;

    // BIT 6 of B
    case 0x70:
      Bit(B, 6);
      break;

    // BIT 6 of C
    case 0x71:
      Bit(C, 6);
      break;

    // BIT 6 of D
    case 0x72:
      Bit(D, 6);
      break;

    // BIT 6 of E
    case 0x73:
      Bit(E, 6);
      break;

    // BIT 6 of H
    case 0x74:
      Bit(H, 6);
      break;

    // BIT 6 of L
    case 0x75:
      Bit(L, 6);
      break;

    // BIT 6 of (HL)
    case 0x76:
      Bit(cpu_read(HL()), 6);
      break;

    // BIT 6 of A
    case 0x77:
      Bit(A, 6);
      break;

    // BIT 7 of B
    case 0x78:
      Bit(B, 7);
      break;

    // BIT 7 of C
    case 0x79:
      Bit(C, 7);
      break;

    // BIT 7 of D
    case 0x7a:
      Bit(D, 7);
      break;

    // BIT 7 of E
    case 0x7b:
      Bit(E, 7);
      break;

    // BIT 7 of H
    case 0x7c:
      Bit(H, 7);
      break;

    // BIT 7 of L
    case 0x7d:
      Bit(L, 7);
      break;

    // BIT 7 of (HL)
    case 0x7e:
      Bit(cpu_read(HL()), 7);
      break;

    // BIT 7 of A
    case 0x7f:
      Bit(A, 7);
      break;

    // RES 0 of B
    case 0x80:
      B = bit_clear(B, 0);
      break;

    // RES 0 of C
    case 0x81:
      C = bit_clear(C, 0);
      break;

    // RES 0 of D
    case 0x82:
      D = bit_clear(D, 0);
      break;

    // RES 0 of E
    case 0x83:
      E = bit_clear(E, 0);
      break;

    // RES 0 of H
    case 0x84:
      H = bit_clear(H, 0);
      break;

    // RES 0 of L
    case 0x85:
      L = bit_clear(L, 0);
      break;

    // RES 0 of (HL)
    case 0x86:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 0));
      break;

    // RES 0 of A
    case 0x87:
      A = bit_clear(A, 0);
      break;

    // RES 1 of B
    case 0x88:
      B = bit_clear(B, 1);
      break;

    // RES 1 of C
    case 0x89:
      C = bit_clear(C, 1);
      break;

    // RES 1 of D
    case 0x8a:
      D = bit_clear(D, 1);
      break;

    // RES 1 of E
    case 0x8b:
      E = bit_clear(E, 1);
      break;

    // RES 1 of H
    case 0x8c:
      H = bit_clear(H, 1);
      break;

    // RES 1 of L
    case 0x8d:
      L = bit_clear(L, 1);
      break;

    // RES 1 of (HL)
    case 0x8e:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 1));
      break;

    // RES 1 of A
    case 0x8f:
      A = bit_clear(A, 1);
      break;

    // RES 2 of B
    case 0x90:
      B = bit_clear(B, 2);
      break;

    // RES 2 of C
    case 0x91:
      C = bit_clear(C, 2);
      break;

    // RES 2 of D
    case 0x92:
      D = bit_clear(D, 2);
      break;

    // RES 2 of E
    case 0x93:
      E = bit_clear(E, 2);
      break;

    // RES 2 of H
    case 0x94:
      H = bit_clear(H, 2);
      break;

    // RES 2 of L
    case 0x95:
      L = bit_clear(L, 2);
      break;

    // RES 2 of (HL)
    case 0x96:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 2));
      break;

    // RES 2 of A
    case 0x97:
      A = bit_clear(A, 2);
      break;

    // RES 3 of B
    case 0x98:
      B = bit_clear(B, 3);
      break;

    // RES 3 of C
    case 0x99:
      C = bit_clear(C, 3);
      break;

    // RES 3 of D
    case 0x9a:
      D = bit_clear(D, 3);
      break;

    // RES 3 of E
    case 0x9b:
      E = bit_clear(E, 3);
      break;

    // RES 3 of H
    case 0x9c:
      H = bit_clear(H, 3);
      break;

    // RES 3 of L
    case 0x9d:
      L = bit_clear(L, 3);
      break;

    // RES 3 of (HL)
    case 0x9e:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 3));
      break;

    // RES 3 of A
    case 0x9f:
      A = bit_clear(A, 3);
      break;

    // RES 4 of B
    case 0xa0:
      B = bit_clear(B, 4);
      break;

    // RES 4 of C
    case 0xa1:
      C = bit_clear(C, 4);
      break;

    // RES 4 of D
    case 0xa2:
      D = bit_clear(D, 4);
      break;

    // RES 4 of E
    case 0xa3:
      E = bit_clear(E, 4);
      break;

    // RES 4 of H
    case 0xa4:
      H = bit_clear(H, 4);
      break;

    // RES 4 of L
    case 0xa5:
      L = bit_clear(L, 4);
      break;

    // RES 4 of (HL)
    case 0xa6:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 4));
      break;

    // RES 4 of A
    case 0xa7:
      A = bit_clear(A, 4);
      break;

    // RES 5 of B
    case 0xa8:
      B = bit_clear(B, 5);
      break;

    // RES 5 of C
    case 0xa9:
      C = bit_clear(C, 5);
      break;

    // RES 5 of D
    case 0xaa:
      D = bit_clear(D, 5);
      break;

    // RES 5 of E
    case 0xab:
      E = bit_clear(E, 5);
      break;

    // RES 5 of H
    case 0xac:
      H = bit_clear(H, 5);
      break;

    // RES 5 of L
    case 0xad:
      L = bit_clear(L, 5);
      break;

    // RES 5 of (HL)
    case 0xae:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 5));
      break;

    // RES 5 of A
    case 0xaf:
      A = bit_clear(A, 5);
      break;

    // RES 6 of B
    case 0xb0:
      B = bit_clear(B, 6);
      break;

    // RES 6 of C
    case 0xb1:
      C = bit_clear(C, 6);
      break;

    // RES 6 of D
    case 0xb2:
      D = bit_clear(D, 6);
      break;

    // RES 6 of E
    case 0xb3:
      E = bit_clear(E, 6);
      break;

    // RES 6 of H
    case 0xb4:
      H = bit_clear(H, 6);
      break;

    // RES 6 of L
    case 0xb5:
      L = bit_clear(L, 6);
      break;

    // RES 6 of (HL)
    case 0xb6:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 6));
      break;

    // RES 6 of A
    case 0xb7:
      A = bit_clear(A, 6);
      break;

    // RES 7 of B
    case 0xb8:
      B = bit_clear(B, 7);
      break;

    // RES 7 of C
    case 0xb9:
      C = bit_clear(C, 7);
      break;

    // RES 7 of D
    case 0xba:
      D = bit_clear(D, 7);
      break;

    // RES 7 of E
    case 0xbb:
      E = bit_clear(E, 7);
      break;

    // RES 7 of H
    case 0xbc:
      H = bit_clear(H, 7);
      break;

    // RES 7 of L
    case 0xbd:
      L = bit_clear(L, 7);
      break;

    // RES 7 of (HL)
    case 0xbe:
      cpu_write(HL(), bit_clear(cpu_read(HL()), 7));
      break;

    // RES 7 of A
    case 0xbf:
      A = bit_clear(A, 7);
      break;

    // SET 0 of B
    case 0xc0:
      B = bit_set(B, 0);
      break;

    // SET 0 of C
    case 0xc1:
      C = bit_set(C, 0);
      break;

    // SET 0 of D
    case 0xc2:
      D = bit_set(D, 0);
      break;

    // SET 0 of E
    case 0xc3:
      E = bit_set(E, 0);
      break;

    // SET 0 of H
    case 0xc4:
      H = bit_set(H, 0);
      break;

    // SET 0 of L
    case 0xc5:
      L = bit_set(L, 0);
      break;

    // SET 0 of (HL)
    case 0xc6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 0));
      break;

    // SET 0 of A
    case 0xc7:
      A = bit_set(A, 0);
      break;

    // SET 1 of B
    case 0xc8:
      B = bit_set(B, 1);
      break;

    // SET 1 of C
    case 0xc9:
      C = bit_set(C, 1);
      break;

    // SET 1 of D
    case 0xca:
      D = bit_set(D, 1);
      break;

    // SET 1 of E
    case 0xcb:
      E = bit_set(E, 1);
      break;

    // SET 1 of H
    case 0xcc:
      H = bit_set(H, 1);
      break;

    // SET 1 of L
    case 0xcd:
      L = bit_set(L, 1);
      break;

    // SET 1 of (HL)
    case 0xce:
      cpu_write(HL(), bit_set(cpu_read(HL()), 1));
      break;

    // SET 1 of A
    case 0xcf:
      A = bit_set(A, 1);
      break;

    // SET 2 of B
    case 0xd0:
      B = bit_set(B, 2);
      break;

    // SET 2 of C
    case 0xd1:
      C = bit_set(C, 2);
      break;

    // SET 2 of D
    case 0xd2:
      D = bit_set(D, 2);
      break;

    // SET 2 of E
    case 0xd3:
      E = bit_set(E, 2);
      break;

    // SET 2 of H
    case 0xd4:
      H = bit_set(H, 2);
      break;

    // SET 2 of L
    case 0xd5:
      L = bit_set(L, 2);
      break;

    // SET 2 of (HL)
    case 0xd6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 2));
      break;

    // SET 2 of A
    case 0xd7:
      A = bit_set(A, 2);
      break;

    // SET 3 of B
    case 0xd8:
      B = bit_set(B, 3);
      break;

    // SET 3 of C
    case 0xd9:
      C = bit_set(C, 3);
      break;

    // SET 3 of D
    case 0xda:
      D = bit_set(D, 3);
      break;

    // SET 3 of E
    case 0xdb:
      E = bit_set(E, 3);
      break;

    // SET 3 of H
    case 0xdc:
      H = bit_set(H, 3);
      break;

    // SET 3 of L
    case 0xdd:
      L = bit_set(L, 3);
      break;

    // SET 3 of (HL)
    case 0xde:
      cpu_write(HL(), bit_set(cpu_read(HL()), 3));
      break;

    // SET 3 of A
    case 0xdf:
      A = bit_set(A, 3);
      break;

    // SET 4 of B
    case 0xe0:
      B = bit_set(B, 4);
      break;

    // SET 4 of C
    case 0xe1:
      C = bit_set(C, 4);
      break;

    // SET 4 of D
    case 0xe2:
      D = bit_set(D, 4);
      break;

    // SET 4 of E
    case 0xe3:
      E = bit_set(E, 4);
      break;

    // SET 4 of H
    case 0xe4:
      H = bit_set(H, 4);
      break;

    // SET 4 of L
    case 0xe5:
      L = bit_set(L, 4);
      break;

    // SET 4 of (HL)
    case 0xe6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 4));
      break;

    // SET 4 of A
    case 0xe7:
      A = bit_set(A, 4);
      break;

    // SET 5 of B
    case 0xe8:
      B = bit_set(B, 5);
      break;

    // SET 5 of C
    case 0xe9:
      C = bit_set(C, 5);
      break;

    // SET 5 of D
    case 0xea:
      D = bit_set(D, 5);
      break;

    // SET 5 of E
    case 0xeb:
      E = bit_set(E, 5);
      break;

    // SET 5 of H
    case 0xec:
      H = bit_set(H, 5);
      break;

    // SET 5 of L
    case 0xed:
      L = bit_set(L, 5);
      break;

    // SET 5 of (HL)
    case 0xee:
      cpu_write(HL(), bit_set(cpu_read(HL()), 5));
      break;

    // SET 5 of A
    case 0xef:
      A = bit_set(A, 5);
      break;

    // SET 6 of B
    case 0xf0:
      B = bit_set(B, 6);
      break;

    // SET 6 of C
    case 0xf1:
      C = bit_set(C, 6);
      break;

    // SET 6 of D
    case 0xf2:
      D = bit_set(D, 6);
      break;

    // SET 6 of E
    case 0xf3:
      E = bit_set(E, 6);
      break;

    // SET 6 of H
    case 0xf4:
      H = bit_set(H, 6);
      break;

    // SET 6 of L
    case 0xf5:
      L = bit_set(L, 6);
      break;

    // SET 6 of (HL)
    case 0xf6:
      cpu_write(HL(), bit_set(cpu_read(HL()), 6));
      break;

    // SET 6 of A
    case 0xf7:
      A = bit_set(A, 6);
      break;

    // SET 7 of B
    case 0xf8:
      B = bit_set(B, 7);
      break;

    // SET 7 of C
    case 0xf9:
      C = bit_set(C, 7);
      break;

    // SET 7 of D
    case 0xfa:
      D = bit_set(D, 7);
      break;

    // SET 7 of E
    case 0xfb:
      E = bit_set(E, 7);
      break;

    // SET 7 of H
    case 0xfc:
      H = bit_set(H, 7);
      break;

    // SET 7 of L
    case 0xfd:
      L = bit_set(L, 7);
      break;

    // SET 7 of (HL)
    case 0xfe:
      cpu_write(HL(), bit_set(cpu_read(HL()), 7));
      break;

    // SET 7 of A
    case 0xff:
      A = bit_set(A, 7);
      break;

// END EX GENERATED

        default:
            printf("Unimplemented EX opcode %x\n", opcode);
            exit(1);
            break;
    }
}

void exec_next(){
    byte op = cpu_read_next();
    //printf("executing %x (%s) at $%x\n", op, opnames[op], PC-1);
    exec_op(op);
}

//
// SDL boilerplate
//

SDL_Window* window;
SDL_Renderer* renderer;
SDL_Texture* texture;

void sdl_init(){
    window = SDL_CreateWindow("Joe's GB", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, VIEWPORT_WIDTH*2, VIEWPORT_HEIGHT*2, SDL_WINDOW_SHOWN);

    if (window == NULL) {
        printf("Window could not be created! SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_SetWindowResizable(window, true);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer == NULL) {
        printf("Renderer could not be created! SDL Error: %s\n", SDL_GetError());
        exit(1);
    }

    surface = SDL_CreateRGBSurface(0, VIEWPORT_WIDTH, VIEWPORT_HEIGHT, 32, 0, 0, 0, 0);
    if (surface == NULL) {
          printf("Surface could not be created! SDL Error: %s\n", SDL_GetError());
          exit(1);
    }
    pixels = (uint32_t*)surface->pixels;
}

void sdl_display(){
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == NULL) {
          printf("Texture could not be created! SDL Error: %s\n", SDL_GetError());
          exit(1);
    }

    // Implement scrolling
    SDL_Rect srcr = {.x = VRAM[SCX], .y = VRAM[SCY], .w = VIEWPORT_WIDTH, .h = VIEWPORT_HEIGHT};

    SDL_RenderCopy(renderer, texture, &srcr, NULL);
    SDL_RenderPresent(renderer);
    SDL_DestroyTexture(texture);
    texture = NULL;
}


bool frame(){
    uint32_t start_ticks = SDL_GetTicks();

    if (bailAfterBios && !inBios) {

    } else {
        cycles = 0;
        while (cycles < 69905) {
            if (bailAfterBios && !inBios) {
                printf("bailing after bios\n");
                break;
        //        return false;
            }
            int prevcycles = cycles;
            exec_next();
            gpu_step(cycles-prevcycles);
        }
    }

    sdl_display();

    uint32_t diff = SDL_GetTicks() - start_ticks;

    if (diff < 1000/FPS) {
        uint32_t nap_time = (1000 / FPS) - diff;
        SDL_Delay(nap_time);
    }

    return true;
}

void sdl_main_impl(void){
  bool run = true;


  SDL_Event event;
  while(run) {
      while (SDL_PollEvent(&event)) {
          if (event.type == SDL_QUIT) {
              printf("got quit event\n");
              run = false;
              return;
          }
      }

      run = frame();
  }

}

int main(){
  cart_load("tetris.gb");
  mem_init();
  cpu_init();
  gpu_init();
  sdl_init();

  sdl_main_impl();

  return 0;
}
