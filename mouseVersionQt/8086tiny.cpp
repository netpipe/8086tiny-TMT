// 8086tiny: a tiny, highly functional, highly portable PC emulator/VM
// Copyright 2013-14, Adrian Cable (adrian.cable@gmail.com) - http://www.megalith.co.uk/8086tiny
//
// Revision 1.25
//
// This work is licensed under the MIT License. See included LICENSE.TXT.

#include <time.h>
#include <sys/timeb.h>
#include <memory.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/timeb.h>

/* Qt 5.12 replaces SDL entirely */
#ifdef QT_PORT
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QImage>
#include <QTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QCursor>
#include <QGuiApplication>
#endif

/* TMT virtual terminal for text-mode output */
#ifdef USE_TMT
//#include "tmt.h"
#endif

/* Original 8086tiny defines — keep these exactly */
#define KEYBOARD_TIMER_UPDATE_DELAY 10000
#define GRAPHICS_X_DEFAULT 640
#define GRAPHICS_Y_DEFAULT 200
#define GRAPHICS_UPDATE_DELAY 1000
#define VTERM_BLANK_LINES 0

/* BIOS and disk files */
#define BIOS_FILE "bios.rom"
#define FD_IMG "fd.img"


#ifndef NO_GRAPHICS
//#include "SDL/SDL.h"
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// For using standard library functions for file io
#include <stdio.h>

// Emulator system constants
#define IO_PORT_COUNT 0x10000
#define RAM_SIZE 0x10FFF0
#define REGS_BASE 0xF0000
#define VIDEO_RAM_SIZE 0x10000

// Graphics/timer/keyboard update delays (explained later)
#ifndef GRAPHICS_UPDATE_DELAY
#define GRAPHICS_UPDATE_DELAY 360000
#endif
#define KEYBOARD_TIMER_UPDATE_DELAY 20000

int is_display_init;

// 16-bit register decodes
#define REG_AX 0
#define REG_CX 1
#define REG_DX 2
#define REG_BX 3
#define REG_SP 4
#define REG_BP 5
#define REG_SI 6
#define REG_DI 7

#define REG_ES 8
#define REG_CS 9
#define REG_SS 10
#define REG_DS 11

#define REG_ZERO 12
#define REG_SCRATCH 13

// 8-bit register decodes
#define REG_AL 0
#define REG_AH 1
#define REG_CL 2
#define REG_CH 3
#define REG_DL 4
#define REG_DH 5
#define REG_BL 6
#define REG_BH 7

// FLAGS register decodes
#define FLAG_CF 40
#define FLAG_PF 41
#define FLAG_AF 42
#define FLAG_ZF 43
#define FLAG_SF 44
#define FLAG_TF 45
#define FLAG_IF 46
#define FLAG_DF 47
#define FLAG_OF 48

// Lookup tables in the BIOS binary
#define TABLE_XLAT_OPCODE 8
#define TABLE_XLAT_SUBFUNCTION 9
#define TABLE_STD_FLAGS 10
#define TABLE_PARITY_FLAG 11
#define TABLE_BASE_INST_SIZE 12
#define TABLE_I_W_SIZE 13
#define TABLE_I_MOD_SIZE 14
#define TABLE_COND_JUMP_DECODE_A 15
#define TABLE_COND_JUMP_DECODE_B 16
#define TABLE_COND_JUMP_DECODE_C 17
#define TABLE_COND_JUMP_DECODE_D 18
#define TABLE_FLAGS_BITFIELDS 19

// Bitfields for TABLE_STD_FLAGS values
#define FLAGS_UPDATE_SZP 1
#define FLAGS_UPDATE_AO_ARITH 2
#define FLAGS_UPDATE_OC_LOGIC 4

// Default values of GRAPHICS_* used to create keyboard listening window in text mode when vterm does not use sdl rendering
#define GRAPHICS_X_DEFAULT 50
#define GRAPHICS_Y_DEFAULT 50

// Emscripten port related
#define EMSCRIPTEN_MAIN_LOOP_FRAMERATE 1000
#ifdef __EMSCRIPTEN__
#define EMSCRIPTEN_INSTRUCTIONS_PER_FRAME 10000
#else
#define EMSCRIPTEN_INSTRUCTIONS_PER_FRAME 1
#endif
// Use hd.img file
//#define EMSCRIPTEN_USE_HD
#define EMSCRIPTEN_BIOS_FILE "bios"
//#define EMSCRIPTEN_FD_FILE "fd.img"
#define EMSCRIPTEN_FD_FILE "ros.img"
#define EMSCRIPTEN_HD_FILE "hd.img"

// Virtual terminal related
#ifdef USE_TMT
#ifdef VTERM_SMALL_CONSOLE
#define VTERM_BLANK_LINES 1
#define VTERM_LINES 9
#define VTERM_COLS 80
#else
#define VTERM_BLANK_LINES 20
#define VTERM_LINES 45
#define VTERM_COLS 80
#endif
#endif

// SDL_PumpEvents and display refreshing can iether be done in every main loop iteration or can be done after a set number of instructions
#ifdef __EMSCRIPTEN__
#define PUMP_EVENTS_EVERY_FRAME
#define REFRESH_DISPLAY_EVERY_FRAME
#endif


// Helper macros

// Decode mod, r_m and reg fields in instruction
#define DECODE_RM_REG scratch2_uint = 4 * !i_mod, \
					  op_to_addr = rm_addr = i_mod < 3 ? SEGREG(seg_override_en ? seg_override : bios_table_lookup[scratch2_uint + 3][i_rm], bios_table_lookup[scratch2_uint][i_rm], regs16[bios_table_lookup[scratch2_uint + 1][i_rm]] + bios_table_lookup[scratch2_uint + 2][i_rm] * i_data1+) : GET_REG_ADDR(i_rm), \
					  op_from_addr = GET_REG_ADDR(i_reg), \
					  i_d && (scratch_uint = op_from_addr, op_from_addr = rm_addr, op_to_addr = scratch_uint)

// Return memory-mapped register location (offset into mem array) for register #reg_id
#define GET_REG_ADDR(reg_id) (REGS_BASE + (i_w ? 2 * reg_id : 2 * reg_id + reg_id / 4 & 7))

// Returns number of top bit in operand (i.e. 8 for 8-bit operands, 16 for 16-bit operands)
#define TOP_BIT 8*(i_w + 1)

// Opcode execution unit helpers
#define OPCODE ;break; case
#define OPCODE_CHAIN ; case

// [I]MUL/[I]DIV/DAA/DAS/ADC/SBB helpers
#define MUL_MACRO(op_data_type,out_regs) (set_opcode(0x10), \
										  out_regs[i_w + 1] = (op_result = CAST(op_data_type)mem[rm_addr] * (op_data_type)*out_regs) >> 16, \
										  regs16[REG_AX] = op_result, \
										  set_OF(set_CF(op_result - (op_data_type)op_result)))
#define DIV_MACRO(out_data_type,in_data_type,out_regs) (scratch_int = CAST(out_data_type)mem[rm_addr]) && !(scratch2_uint = (in_data_type)(scratch_uint = (out_regs[i_w+1] << 16) + regs16[REG_AX]) / scratch_int, scratch2_uint - (out_data_type)scratch2_uint) ? out_regs[i_w+1] = scratch_uint - scratch_int * (*out_regs = scratch2_uint) : pc_interrupt(0)
#define DAA_DAS(op1,op2,mask,min) set_AF((((scratch2_uint = regs8[REG_AL]) & 0x0F) > 9) || regs8[FLAG_AF]) && (op_result = regs8[REG_AL] op1 6, set_CF(regs8[FLAG_CF] || (regs8[REG_AL] op2 scratch2_uint))), \
								  set_CF((((mask & 1 ? scratch2_uint : regs8[REG_AL]) & mask) > min) || regs8[FLAG_CF]) && (op_result = regs8[REG_AL] op1 0x60)
#define ADC_SBB_MACRO(a) OP(a##= regs8[FLAG_CF] +), \
						 set_CF(regs8[FLAG_CF] && (op_result == op_dest) || (a op_result < a(int)op_dest)), \
						 set_AF_OF_arith()

// Execute arithmetic/logic operations in emulator memory/registers
#define R_M_OP(dest,op,src) (i_w ? op_dest = CAST(unsigned short)dest, op_result = CAST(unsigned short)dest op (op_source = CAST(unsigned short)src) \
								 : (op_dest = dest, op_result = dest op (op_source = CAST(unsigned char)src)))
#define MEM_OP(dest,op,src) R_M_OP(mem[dest],op,mem[src])
#define OP(op) MEM_OP(op_to_addr,op,op_from_addr)

// Increment or decrement a register #reg_id (usually SI or DI), depending on direction flag and operand size (given by i_w)
#define INDEX_INC(reg_id) (regs16[reg_id] -= (2 * regs8[FLAG_DF] - 1)*(i_w + 1))

// Helpers for stack operations
#define R_M_PUSH(a) (i_w = 1, R_M_OP(mem[SEGREG(REG_SS, REG_SP, --)], =, a))
#define R_M_POP(a) (i_w = 1, regs16[REG_SP] += 2, R_M_OP(a, =, mem[SEGREG(REG_SS, REG_SP, -2+)]))

// Convert segment:offset to linear address in emulator memory space
#define SEGREG(reg_seg,reg_ofs,op) 16 * regs16[reg_seg] + (unsigned short)(op regs16[reg_ofs])

// Returns sign bit of an 8-bit or 16-bit operand
#define SIGN_OF(a) (1 & (i_w ? CAST(short)a : a) >> (TOP_BIT - 1))

// Reinterpretation cast
#define CAST(a) *(a*)&

// Keyboard driver for console. This may need changing for UNIX/non-UNIX platforms
#ifdef _WIN32
#define KEYBOARD_DRIVER kbhit() && (mem[0x4A6] = getch(), pc_interrupt(7))
#else
#define KEYBOARD_DRIVER read(0, mem + 0x4A6, 1) && (int8_asap = (mem[0x4A6] == 0x1B), pc_interrupt(7))
#endif

// Global variable definitions
unsigned char mem[RAM_SIZE], io_ports[IO_PORT_COUNT], *opcode_stream, *regs8, i_rm, i_w, i_reg, i_mod, i_mod_size, i_d, i_reg4bit, raw_opcode_id, xlat_opcode_id, extra, rep_mode, seg_override_en, rep_override_en, trap_flag, int8_asap, scratch_uchar, io_hi_lo, *vid_mem_base, spkr_en, bios_table_lookup[20][256];
unsigned short *regs16, reg_ip, seg_override, file_index, wave_counter;
unsigned int op_source, op_dest, rm_addr, op_to_addr, op_from_addr, i_data0, i_data1, i_data2, scratch_uint, scratch2_uint, scratch3_uint, inst_counter, set_flags_type, GRAPHICS_X, GRAPHICS_Y, pixel_colors[16], vmem_ctr;
int op_result, disk_size, scratch_int;
time_t clock_buf;
struct timeb ms_clock;
FILE* disk[3];

#ifndef NO_GRAPHICS
#ifndef NO_AUDIO
SDL_AudioSpec sdl_audio = {44100, AUDIO_U8, 1, 0, 128};
#endif
#ifdef QT_PORT
QImage qt_image(GRAPHICS_X_DEFAULT, GRAPHICS_Y_DEFAULT, QImage::Format_RGB32);
QMutex qt_image_mutex;
QWidget *qt_widget = nullptr;
bool qt_screen_dirty = false;
#endif
unsigned short vid_addr_lookup[VIDEO_RAM_SIZE], cga_colors[4] = {0 /* Black */, 0x1F1F /* Cyan */, 0xE3E3 /* Magenta */, 0xFFFF /* White */};
#endif

// Virtual terminal via tmt
#ifdef USE_TMT
TMT* vterm;
int vterm_needs_draw;
#endif

// Should main loop continue
int cont_main_loop;

// INT33 mouse emulation (host-backed)
// 
// Mouse state storage:
// SDL mouse events update this structure, which is then read by INT 33h handlers.
// Coordinates are stored in DOS guest space (e.g., 0-639 x 0-199 for 640x200 mode).
// Button state is a bitmask: bit 0 = left button, bit 1 = right button.
static struct {
    int x;                   // X position (guest coordinates)
    int y;                   // Y position (guest coordinates)
    int buttons;             // Button bitmask: bit 0 = left, bit 1 = right
    int visible;             // Visibility flag (1 = visible, 0 = hidden)
#ifndef NO_GRAPHICS
    // Coordinate mapping fields (only needed when graphics enabled)
    int guest_w, guest_h;   // guest coordinate area (e.g. 320x200 or 640x200)
    int host_w, host_h;     // SDL window size
#endif
} emu_mouse;

// Initialize mouse state with reasonable defaults
static void emu_mouse_init(int gw, int gh)
{
    // Initialize core internal mouse state
    emu_mouse.x = gw / 2;           // Start at center X
    emu_mouse.y = gh / 2;           // Start at center Y
    emu_mouse.buttons = 0;          // No buttons pressed
    emu_mouse.visible = 1;          // Mouse cursor visible by default
#ifndef NO_GRAPHICS
    // Initialize coordinate mapping fields
    emu_mouse.guest_w = gw;
    emu_mouse.guest_h = gh;
    emu_mouse.host_w = GRAPHICS_X_DEFAULT;
    emu_mouse.host_h = GRAPHICS_Y_DEFAULT;
#endif
}

// Map SDL mouse coordinates to DOS coordinate space
// This ensures consistent coordinate mapping across all mouse event sources
static void emu_mouse_map_coords(int host_x, int host_y)
{
#ifndef NO_GRAPHICS
    // Update host window dimensions if available
    //if (sdl_screen) {
      //  emu_mouse.host_w = sdl_screen->w;
     //   emu_mouse.host_h = sdl_screen->h;
   // }
    
    // Only map if we have valid coordinate spaces
    if (emu_mouse.host_w > 0 && emu_mouse.host_h > 0 && 
        emu_mouse.guest_w > 0 && emu_mouse.guest_h > 0) {
        
        // Clamp host coordinates to window bounds (safe clamping)
        int mx = host_x;
        int my = host_y;
        
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= emu_mouse.host_w) mx = emu_mouse.host_w - 1;
        if (my >= emu_mouse.host_h) my = emu_mouse.host_h - 1;
        
        // Map from host (SDL window) coordinates to guest (DOS) coordinates
        // Use integer division for consistent behavior
        emu_mouse.x = (mx * emu_mouse.guest_w) / emu_mouse.host_w;
        emu_mouse.y = (my * emu_mouse.guest_h) / emu_mouse.host_h;
        
        // Final clamp to ensure guest coordinates are within valid bounds
        if (emu_mouse.x < 0) emu_mouse.x = 0;
        if (emu_mouse.y < 0) emu_mouse.y = 0;
        if (emu_mouse.x >= emu_mouse.guest_w) emu_mouse.x = emu_mouse.guest_w - 1;
        if (emu_mouse.y >= emu_mouse.guest_h) emu_mouse.y = emu_mouse.guest_h - 1;
    }
#endif
}

// Poll SDL mouse state and map into guest coords
// Only processes mouse input when SDL window is active
static void emu_mouse_poll_from_host()
{
#ifdef QT_PORT
    if (!qt_widget) return;
    QPoint globalPos = QCursor::pos();
    QPoint localPos = qt_widget->mapFromGlobal(globalPos);
    emu_mouse_map_coords(localPos.x(), localPos.y());

    Qt::MouseButtons btns = QGuiApplication::mouseButtons();
    emu_mouse.buttons = 0;
    if (btns & Qt::LeftButton)  emu_mouse.buttons |= 1;
    if (btns & Qt::RightButton) emu_mouse.buttons |= 2;
#else
    int mx, my;
    Uint8 sdl_buttons = SDL_GetMouseState(&mx, &my);
    emu_mouse_map_coords(mx, my);
    emu_mouse.buttons = 0;
    if (sdl_buttons & SDL_BUTTON(SDL_BUTTON_LEFT))  emu_mouse.buttons |= 1;
    if (sdl_buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) emu_mouse.buttons |= 2;
#endif
}

// Handle INT 33h mouse interrupt
// 
// INT 33h returns mouse data to DOS programs:
// DOS programs call INT 33h with function code in AX. This handler reads from
// emu_mouse state structure and returns data in CPU registers (BX, CX, DX).
// For example, function 0x0003 returns button state in BX, X in CX, Y in DX.
#ifndef NO_GRAPHICS
static void emulate_int33()
{
    unsigned short ax = regs16[REG_AX];
    unsigned short bx = regs16[REG_BX];
    unsigned short cx = regs16[REG_CX];
    unsigned short dx = regs16[REG_DX];

    switch (ax) {
    case 0x0000:
        // Reset / detect mouse driver
        // Return AX = 0xFFFF (success) and BX = number of buttons (2)
        regs16[REG_AX] = 0xFFFF;
        regs16[REG_BX] = 2;
        break;

    case 0x0001:
        // Show mouse cursor - set internal visibility flag
        emu_mouse.visible = 1;
        regs16[REG_AX] = 0;
        break;

    case 0x0002:
        // Hide mouse cursor - clear visibility flag
        emu_mouse.visible = 0;
        regs16[REG_AX] = 0;
        break;

    case 0x0003:
        // Get mouse position and button status
        // Return BX = button bitmask, CX = mouse X, DX = mouse Y
        regs16[REG_BX] = (unsigned short)emu_mouse.buttons;
        regs16[REG_CX] = (unsigned short)emu_mouse.x;
        regs16[REG_DX] = (unsigned short)emu_mouse.y;
        regs16[REG_AX] = 0;
        break;

    case 0x0004:
        // Set mouse position - read CX/DX and update internal mouse state
        emu_mouse.x = cx;
        emu_mouse.y = dx;
        // Clamp to valid bounds
        if (emu_mouse.x < 0) emu_mouse.x = 0;
        if (emu_mouse.y < 0) emu_mouse.y = 0;
        if (emu_mouse.x >= emu_mouse.guest_w) emu_mouse.x = emu_mouse.guest_w - 1;
        if (emu_mouse.y >= emu_mouse.guest_h) emu_mouse.y = emu_mouse.guest_h - 1;
        regs16[REG_AX] = 0;
        break;

    case 0x0005:
        // Get button press information
        // BX = button number (0=left, 1=right)
        // Returns: AX = button status, BX = press count, CX = X at last press, DX = Y at last press
        {
            unsigned short btnnum = bx;
            // Return current button state (minimal implementation, no event queue)
            regs16[REG_AX] = (unsigned short)emu_mouse.buttons;
            regs16[REG_BX] = 0; // Press count (not implemented - no event queue)
            regs16[REG_CX] = (unsigned short)emu_mouse.x; // Current X position
            regs16[REG_DX] = (unsigned short)emu_mouse.y; // Current Y position
        }
        break;

    case 0x0006:
        // Get button release information
        // BX = button number (0=left, 1=right)
        // Returns: AX = button status, BX = release count, CX = X at last release, DX = Y at last release
        {
            unsigned short btnnum = bx;
            // Return current button state (minimal implementation, no event queue)
            regs16[REG_AX] = (unsigned short)emu_mouse.buttons;
            regs16[REG_BX] = 0; // Release count (not implemented - no event queue)
            regs16[REG_CX] = (unsigned short)emu_mouse.x; // Current X position
            regs16[REG_DX] = (unsigned short)emu_mouse.y; // Current Y position
        }
        break;

    case 0x000B:
        // Get button press/release counts and status
        // BX = button number (0=left,1=right)
        // Returns CX=pressCount DX=releaseCount
        {
            unsigned short btnnum = bx;
            unsigned short status = 0;
            if (btnnum == 0 && (emu_mouse.buttons & 1)) status = 1;
            if (btnnum == 1 && (emu_mouse.buttons & 2)) status = 1;
            regs16[REG_AX] = status;
            regs16[REG_CX] = 0; // press count (not implemented)
            regs16[REG_DX] = 0; // release count
        }
        break;

    default:
        // Unsupported INT33 function - return AX=0 (failure)
        regs16[REG_AX] = 0;
        break;
    }
}
#endif

// Helper functions

// Set carry flag
char set_CF(int new_CF)
{
	return regs8[FLAG_CF] = !!new_CF;
}

// Set auxiliary flag
char set_AF(int new_AF)
{
	return regs8[FLAG_AF] = !!new_AF;
}

// Set overflow flag
char set_OF(int new_OF)
{
	return regs8[FLAG_OF] = !!new_OF;
}

// Set auxiliary and overflow flag after arithmetic operations
char set_AF_OF_arith()
{
	set_AF((op_source ^= op_dest ^ op_result) & 0x10);
	if (op_result == op_dest)
		return set_OF(0);
	else
		return set_OF(1 & (regs8[FLAG_CF] ^ op_source >> (TOP_BIT - 1)));
}

// Assemble and return emulated CPU FLAGS register in scratch_uint
void make_flags()
{
	scratch_uint = 0xF002; // 8086 has reserved and unused flags set to 1
	for (int i = 9; i--;)
		scratch_uint += regs8[FLAG_CF + i] << bios_table_lookup[TABLE_FLAGS_BITFIELDS][i];
}

// Set emulated CPU FLAGS register from regs8[FLAG_xx] values
void set_flags(int new_flags)
{
	for (int i = 9; i--;)
		regs8[FLAG_CF + i] = !!(1 << bios_table_lookup[TABLE_FLAGS_BITFIELDS][i] & new_flags);
}

// Convert raw opcode to translated opcode index. This condenses a large number of different encodings of similar
// instructions into a much smaller number of distinct functions, which we then execute
void set_opcode(unsigned char opcode)
{
	xlat_opcode_id = bios_table_lookup[TABLE_XLAT_OPCODE][raw_opcode_id = opcode];
	extra = bios_table_lookup[TABLE_XLAT_SUBFUNCTION][opcode];
	i_mod_size = bios_table_lookup[TABLE_I_MOD_SIZE][opcode];
	set_flags_type = bios_table_lookup[TABLE_STD_FLAGS][opcode];
}

// Execute INT #interrupt_num on the emulated machine
char pc_interrupt(unsigned char interrupt_num)
{
	set_opcode(0xCD); // Decode like INT

	make_flags();
	R_M_PUSH(scratch_uint);
	R_M_PUSH(regs16[REG_CS]);
	R_M_PUSH(reg_ip);
	MEM_OP(REGS_BASE + 2 * REG_CS, =, 4 * interrupt_num + 2);
	R_M_OP(reg_ip, =, mem[4 * interrupt_num]);

	return regs8[FLAG_TF] = regs8[FLAG_IF] = 0;
}

// AAA and AAS instructions - which_operation is +1 for AAA, and -1 for AAS
int AAA_AAS(char which_operation)
{
	return (regs16[REG_AX] += 262 * which_operation*set_AF(set_CF(((regs8[REG_AL] & 0x0F) > 9) || regs8[FLAG_AF])), regs8[REG_AL] &= 0x0F);
}

#ifndef NO_AUDIO
#ifndef NO_GRAPHICS
void audio_callback(void *data, unsigned char *stream, int len)
{
	for (int i = 0; i < len; i++)
		stream[i] = (spkr_en == 3) && CAST(unsigned short)mem[0x4AA] ? -((54 * wave_counter++ / CAST(unsigned short)mem[0x4AA]) & 1) : sdl_audio.silence;

	spkr_en = io_ports[0x61] & 3;
}
#endif
#endif

// Callback for tmt
#ifdef USE_TMT
void tmt_callback(tmt_msg_t m, TMT* vt, void const *a, void *p)
{
}
#endif

// Translates SDLKey to ASCII. Keysyms match ASCII only if SDL on the platform defines it that way.
#ifdef QT_PORT
/* Map Qt::Key values to the scan codes 8086tiny expects */
unsigned int qt_key_to_ascii(int key) {
    /* Letters */
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return key - Qt::Key_A + 'a';
    /* Numbers */
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return key - Qt::Key_0 + '0';
    /* Common keys matching SDL keycodes */
    switch(key) {
        case Qt::Key_Backspace:   return 8;
        case Qt::Key_Tab:         return 9;
        case Qt::Key_Return:      return 13;
        case Qt::Key_Enter:       return 13;
        case Qt::Key_Escape:      return 27;
        case Qt::Key_Space:       return 32;
        case Qt::Key_Exclam:      return 33;
        case Qt::Key_QuoteDbl:    return 34;
        case Qt::Key_NumberSign:  return 35;
        case Qt::Key_Dollar:      return 36;
        case Qt::Key_Ampersand:   return 38;
        case Qt::Key_Apostrophe:  return 39;
        case Qt::Key_ParenLeft:   return 40;
        case Qt::Key_ParenRight:  return 41;
        case Qt::Key_Asterisk:    return 42;
        case Qt::Key_Plus:        return 43;
        case Qt::Key_Comma:       return 44;
        case Qt::Key_Minus:       return 45;
        case Qt::Key_Period:      return 46;
        case Qt::Key_Slash:       return 47;
        case Qt::Key_Colon:       return 58;
        case Qt::Key_Semicolon:   return 59;
        case Qt::Key_Less:        return 60;
        case Qt::Key_Equal:       return 61;
        case Qt::Key_Greater:     return 62;
        case Qt::Key_Question:    return 63;
        case Qt::Key_At:          return 64;
        case Qt::Key_BracketLeft: return 91;
        case Qt::Key_Backslash:   return 92;
        case Qt::Key_BracketRight:return 93;
        case Qt::Key_AsciiCircum: return 94;
        case Qt::Key_Underscore:  return 95;
        case Qt::Key_QuoteLeft:   return 96;
        /* Arrow keys — 8086tiny expects SDL-style codes */
        case Qt::Key_Up:          return 273;
        case Qt::Key_Down:        return 274;
        case Qt::Key_Right:       return 275;
        case Qt::Key_Left:        return 276;
        case Qt::Key_Insert:      return 277;
        case Qt::Key_Delete:      return 127;
        case Qt::Key_Home:        return 278;
        case Qt::Key_End:         return 279;
        case Qt::Key_PageUp:      return 280;
        case Qt::Key_PageDown:    return 281;
        /* Function keys */
        case Qt::Key_F1:          return 282;
        case Qt::Key_F2:          return 283;
        case Qt::Key_F3:          return 284;
        case Qt::Key_F4:          return 285;
        case Qt::Key_F5:          return 286;
        case Qt::Key_F6:          return 287;
        case Qt::Key_F7:          return 288;
        case Qt::Key_F8:          return 289;
        case Qt::Key_F9:          return 290;
        case Qt::Key_F10:         return 291;
        case Qt::Key_F11:         return 292;
        case Qt::Key_F12:         return 293;
    }
    return 0;
}
#endif

#ifndef NO_GRAPHICS
void set_video_mode()
{
#ifdef QT_PORT
    /* Resize the QImage to match the current graphics mode */
    qt_image_mutex.lock();
    qt_image = QImage(GRAPHICS_X, GRAPHICS_Y, QImage::Format_RGB32);
    qt_image.fill(0xFF000000);
    qt_image_mutex.unlock();
    /* Update mouse host dimensions to match window */
    emu_mouse.host_w = GRAPHICS_X;
    emu_mouse.host_h = GRAPHICS_Y;
#else
    SDL_Init(SDL_INIT_VIDEO);
    sdl_screen = SDL_SetVideoMode(GRAPHICS_X, GRAPHICS_Y, 32, SDL_SWSURFACE);
    sdl_fmt = sdl_screen->format;
#endif
}
#endif

void init(int argc, char** argv)
{
    FILE *f;
    unsigned char *c;
    int file_index = 0;

    /* Initialize mouse state — from mouseVersion */
    emu_mouse.x = 320;
    emu_mouse.y = 100;
    emu_mouse.buttons = 0;
    emu_mouse.visible = 1;
    emu_mouse.guest_w = 640;
    emu_mouse.guest_h = 200;
    emu_mouse.host_w = GRAPHICS_X_DEFAULT;
    emu_mouse.host_h = GRAPHICS_Y_DEFAULT;

    /* Reset emulator state */
    memset(mem, 0, sizeof(mem));
    memset(io_ports, 0, sizeof(io_ports));
    memset(regs8, 0, sizeof(regs8));

    /* ---- LOAD BIOS ROM (KEEP THIS EXACTLY) ---- */
    if (!(f = fopen(BIOS_FILE, "rb"))) {
        fprintf(stderr, "Cannot open %s\n", BIOS_FILE);
        exit(1);
    }
    fread(mem + 0xF0000, 1, 0x10000, f);
    fclose(f);

    /* Copy BIOS tables for opcode decoding */
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 256; j++)
            bios_table_lookup[i][j] = mem[0xFE000 + i * 256 + j];

    /* ---- LOAD FLOPPY DISK IMAGE (KEEP THIS EXACTLY) ---- */
    if (!(disk[0] = fopen(FD_IMG, "r+b"))) {
        fprintf(stderr, "Cannot open %s\n", FD_IMG);
        exit(1);
    }

    /* Set CX:AX equal to the hard disk image size */
    CAST(unsigned)regs16[REG_AX] = *disk ? fseek(*disk, 0, SEEK_END) : 0;

    /* Reset CPU registers */
    memset(regs16, 0, sizeof(regs16));
    regs16[REG_CS] = 0xF000;
    reg_ip = 0xFFF0;
    regs8[FLAG_TF] = 0;

    /* Set DL to boot device (0 = floppy) */
    regs8[REG_DL] = 0;

    /* Set up interrupt vectors */
    for (int i = 0; i < 256; i++) {
        mem[i * 4] = 0;
        mem[i * 4 + 1] = 0;
        mem[i * 4 + 2] = 0;
        mem[i * 4 + 3] = 0;
    }

    /* Video mode setup */
    GRAPHICS_X = GRAPHICS_X_DEFAULT;
    GRAPHICS_Y = GRAPHICS_Y_DEFAULT;
    set_video_mode();
    is_display_init = 0;

    /* Initialize mouse coordinate mapping */
    emu_mouse_init(640, 200);

    /* Set main loop to continue */
    cont_main_loop = 1;
}

void quit()
{
#ifdef USE_TMT
    tmt_close(vterm);
#endif
#ifndef NO_GRAPHICS
//	SDL_Quit();
#endif
}


void main_loop()
{
    // Instruction execution loop. Loops for EMSCRIPTEN_INSTRUCTIONS_PER_FRAME. Terminates if CS:IP = 0:0
	for (int i = 0; i < EMSCRIPTEN_INSTRUCTIONS_PER_FRAME && cont_main_loop == 1; i++)
	{

        if(!((opcode_stream = mem + 16 * regs16[REG_CS] + reg_ip) && (opcode_stream != mem)))
        {
            // Quit logic
#ifdef __EMSCRIPTEN__
            emscripten_cancel_main_loop();
            quit();
#endif
            cont_main_loop = 0;
            break;
        }
		// Set up variables to prepare for decoding an opcode
		set_opcode(*opcode_stream);

		// Extract i_w and i_d fields from instruction
		i_w = (i_reg4bit = raw_opcode_id & 7) & 1;
		i_d = i_reg4bit / 2 & 1;

		// Extract instruction data fields
		i_data0 = CAST(short)opcode_stream[1];
		i_data1 = CAST(short)opcode_stream[2];
		i_data2 = CAST(short)opcode_stream[3];

		// seg_override_en and rep_override_en contain number of instructions to hold segment override and REP prefix respectively
		if (seg_override_en)
			seg_override_en--;
		if (rep_override_en)
			rep_override_en--;

		// i_mod_size > 0 indicates that opcode uses i_mod/i_rm/i_reg, so decode them
		if (i_mod_size)
		{
			i_mod = (i_data0 & 0xFF) >> 6;
			i_rm = i_data0 & 7;
			i_reg = i_data0 / 8 & 7;

			if ((!i_mod && i_rm == 6) || (i_mod == 2))
				i_data2 = CAST(short)opcode_stream[4];
			else if (i_mod != 1)
				i_data2 = i_data1;
			else // If i_mod is 1, operand is (usually) 8 bits rather than 16 bits
				i_data1 = (char)i_data1;

			DECODE_RM_REG;
		}

		// Instruction execution unit
		switch (xlat_opcode_id)
		{
			OPCODE_CHAIN 0: // Conditional jump (JAE, JNAE, etc.)
				// i_w is the invert flag, e.g. i_w == 1 means JNAE, whereas i_w == 0 means JAE
				scratch_uchar = raw_opcode_id / 2 & 7;
				reg_ip += (char)i_data0 * (i_w ^ (regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_A][scratch_uchar]] || regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_B][scratch_uchar]] || regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_C][scratch_uchar]] ^ regs8[bios_table_lookup[TABLE_COND_JUMP_DECODE_D][scratch_uchar]]))
			OPCODE 1: // MOV reg, imm
				i_w = !!(raw_opcode_id & 8);
				R_M_OP(mem[GET_REG_ADDR(i_reg4bit)], =, i_data0)
			OPCODE 3: // PUSH regs16
				R_M_PUSH(regs16[i_reg4bit])
			OPCODE 4: // POP regs16
				R_M_POP(regs16[i_reg4bit])
			OPCODE 2: // INC|DEC regs16
				i_w = 1;
				i_d = 0;
				i_reg = i_reg4bit;
				DECODE_RM_REG;
				i_reg = extra
			OPCODE_CHAIN 5: // INC|DEC|JMP|CALL|PUSH
				if (i_reg < 2) // INC|DEC
					MEM_OP(op_from_addr, += 1 - 2 * i_reg +, REGS_BASE + 2 * REG_ZERO),
					op_source = 1,
					set_AF_OF_arith(),
					set_OF(op_dest + 1 - i_reg == 1 << (TOP_BIT - 1)),
					(xlat_opcode_id == 5) && (set_opcode(0x10), 0); // Decode like ADC
				else if (i_reg != 6) // JMP|CALL
					i_reg - 3 || R_M_PUSH(regs16[REG_CS]), // CALL (far)
					i_reg & 2 && R_M_PUSH(reg_ip + 2 + i_mod*(i_mod != 3) + 2*(!i_mod && i_rm == 6)), // CALL (near or far)
					i_reg & 1 && (regs16[REG_CS] = CAST(short)mem[op_from_addr + 2]), // JMP|CALL (far)
					R_M_OP(reg_ip, =, mem[op_from_addr]),
					set_opcode(0x9A); // Decode like CALL
				else // PUSH
					R_M_PUSH(mem[rm_addr])
			OPCODE 6: // TEST r/m, imm16 / NOT|NEG|MUL|IMUL|DIV|IDIV reg
				op_to_addr = op_from_addr;

				switch (i_reg)
				{
					OPCODE_CHAIN 0: // TEST
						set_opcode(0x20); // Decode like AND
						reg_ip += i_w + 1;
						R_M_OP(mem[op_to_addr], &, i_data2)
					OPCODE 2: // NOT
						OP(=~)
					OPCODE 3: // NEG
						OP(=-);
						op_dest = 0;
						set_opcode(0x28); // Decode like SUB
						set_CF(op_result > op_dest)
					OPCODE 4: // MUL
						i_w ? MUL_MACRO(unsigned short, regs16) : MUL_MACRO(unsigned char, regs8)
					OPCODE 5: // IMUL
						i_w ? MUL_MACRO(short, regs16) : MUL_MACRO(char, regs8)
					OPCODE 6: // DIV
						i_w ? DIV_MACRO(unsigned short, unsigned, regs16) : DIV_MACRO(unsigned char, unsigned short, regs8)
					OPCODE 7: // IDIV
						i_w ? DIV_MACRO(short, int, regs16) : DIV_MACRO(char, short, regs8);
				}
			OPCODE 7: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP AL/AX, immed
				rm_addr = REGS_BASE;
				i_data2 = i_data0;
				i_mod = 3;
				i_reg = extra;
				reg_ip--;
			OPCODE_CHAIN 8: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP reg, immed
				op_to_addr = rm_addr;
				regs16[REG_SCRATCH] = (i_d |= !i_w) ? (char)i_data2 : i_data2;
				op_from_addr = REGS_BASE + 2 * REG_SCRATCH;
				reg_ip += !i_d + 1;
				set_opcode(0x08 * (extra = i_reg));
			OPCODE_CHAIN 9: // ADD|OR|ADC|SBB|AND|SUB|XOR|CMP|MOV reg, r/m
				switch (extra)
				{
					OPCODE_CHAIN 0: // ADD
						OP(+=),
						set_CF(op_result < op_dest)
					OPCODE 1: // OR
						OP(|=)
					OPCODE 2: // ADC
						ADC_SBB_MACRO(+)
					OPCODE 3: // SBB
						ADC_SBB_MACRO(-)
					OPCODE 4: // AND
						OP(&=)
					OPCODE 5: // SUB
						OP(-=),
						set_CF(op_result > op_dest)
					OPCODE 6: // XOR
						OP(^=)
					OPCODE 7: // CMP
						OP(-),
						set_CF(op_result > op_dest)
					OPCODE 8: // MOV
						OP(=);
				}
			OPCODE 10: // MOV sreg, r/m | POP r/m | LEA reg, r/m
				if (!i_w) // MOV
					i_w = 1,
					i_reg += 8,
					DECODE_RM_REG,
					OP(=);
				else if (!i_d) // LEA
					seg_override_en = 1,
					seg_override = REG_ZERO,
					DECODE_RM_REG,
					R_M_OP(mem[op_from_addr], =, rm_addr);
				else // POP
					R_M_POP(mem[rm_addr])
			OPCODE 11: // MOV AL/AX, [loc]
				i_mod = i_reg = 0;
				i_rm = 6;
				i_data1 = i_data0;
				DECODE_RM_REG;
				MEM_OP(op_from_addr, =, op_to_addr)
			OPCODE 12: // ROL|ROR|RCL|RCR|SHL|SHR|???|SAR reg/mem, 1/CL/imm (80186)
				scratch2_uint = SIGN_OF(mem[rm_addr]),
				scratch_uint = extra ? // xxx reg/mem, imm
					++reg_ip,
					(char)i_data1
				: // xxx reg/mem, CL
					i_d
						? 31 & regs8[REG_CL]
				: // xxx reg/mem, 1
					1;
				if (scratch_uint)
				{
					if (i_reg < 4) // Rotate operations
						scratch_uint %= i_reg / 2 + TOP_BIT,
						R_M_OP(scratch2_uint, =, mem[rm_addr]);
					if (i_reg & 1) // Rotate/shift right operations
						R_M_OP(mem[rm_addr], >>=, scratch_uint);
					else // Rotate/shift left operations
						R_M_OP(mem[rm_addr], <<=, scratch_uint);
					if (i_reg > 3) // Shift operations
						set_opcode(0x10); // Decode like ADC
					if (i_reg > 4) // SHR or SAR
						set_CF(op_dest >> (scratch_uint - 1) & 1);
				}

				switch (i_reg)
				{
					OPCODE_CHAIN 0: // ROL
						R_M_OP(mem[rm_addr], += , scratch2_uint >> (TOP_BIT - scratch_uint));
						set_OF(SIGN_OF(op_result) ^ set_CF(op_result & 1))
					OPCODE 1: // ROR
						scratch2_uint &= (1 << scratch_uint) - 1,
						R_M_OP(mem[rm_addr], += , scratch2_uint << (TOP_BIT - scratch_uint));
						set_OF(SIGN_OF(op_result * 2) ^ set_CF(SIGN_OF(op_result)))
					OPCODE 2: // RCL
						R_M_OP(mem[rm_addr], += (regs8[FLAG_CF] << (scratch_uint - 1)) + , scratch2_uint >> (1 + TOP_BIT - scratch_uint));
						set_OF(SIGN_OF(op_result) ^ set_CF(scratch2_uint & 1 << (TOP_BIT - scratch_uint)))
					OPCODE 3: // RCR
						R_M_OP(mem[rm_addr], += (regs8[FLAG_CF] << (TOP_BIT - scratch_uint)) + , scratch2_uint << (1 + TOP_BIT - scratch_uint));
						set_CF(scratch2_uint & 1 << (scratch_uint - 1));
						set_OF(SIGN_OF(op_result) ^ SIGN_OF(op_result * 2))
					OPCODE 4: // SHL
						set_OF(SIGN_OF(op_result) ^ set_CF(SIGN_OF(op_dest << (scratch_uint - 1))))
					OPCODE 5: // SHR
						set_OF(SIGN_OF(op_dest))
					OPCODE 7: // SAR
						scratch_uint < TOP_BIT || set_CF(scratch2_uint);
						set_OF(0);
						R_M_OP(mem[rm_addr], +=, scratch2_uint *= ~(((1 << TOP_BIT) - 1) >> scratch_uint));
				}
			OPCODE 13: // LOOPxx|JCZX
				scratch_uint = !!--regs16[REG_CX];

				switch(i_reg4bit)
				{
					OPCODE_CHAIN 0: // LOOPNZ
						scratch_uint &= !regs8[FLAG_ZF]
					OPCODE 1: // LOOPZ
						scratch_uint &= regs8[FLAG_ZF]
					OPCODE 3: // JCXXZ
						scratch_uint = !++regs16[REG_CX];
				}
				reg_ip += scratch_uint*(char)i_data0
			OPCODE 14: // JMP | CALL short/near
				reg_ip += 3 - i_d;
				if (!i_w)
				{
					if (i_d) // JMP far
						reg_ip = 0,
						regs16[REG_CS] = i_data2;
					else // CALL
						R_M_PUSH(reg_ip);
				}
				reg_ip += i_d && i_w ? (char)i_data0 : i_data0
			OPCODE 15: // TEST reg, r/m
				MEM_OP(op_from_addr, &, op_to_addr)
			OPCODE 16: // XCHG AX, regs16
				i_w = 1;
				op_to_addr = REGS_BASE;
				op_from_addr = GET_REG_ADDR(i_reg4bit);
			OPCODE_CHAIN 24: // NOP|XCHG reg, r/m
				if (op_to_addr != op_from_addr)
					OP(^=),
					MEM_OP(op_from_addr, ^=, op_to_addr),
					OP(^=)
			OPCODE 17: // MOVSx (extra=0)|STOSx (extra=1)|LODSx (extra=2)
				scratch2_uint = seg_override_en ? seg_override : REG_DS;

				for (scratch_uint = rep_override_en ? regs16[REG_CX] : 1; scratch_uint; scratch_uint--)
				{
					MEM_OP(extra < 2 ? SEGREG(REG_ES, REG_DI,) : REGS_BASE, =, extra & 1 ? REGS_BASE : SEGREG(scratch2_uint, REG_SI,)),
					extra & 1 || INDEX_INC(REG_SI),
					extra & 2 || INDEX_INC(REG_DI);
				}

				if (rep_override_en)
					regs16[REG_CX] = 0
			OPCODE 18: // CMPSx (extra=0)|SCASx (extra=1)
				scratch2_uint = seg_override_en ? seg_override : REG_DS;

				if ((scratch_uint = rep_override_en ? regs16[REG_CX] : 1))
				{
					for (; scratch_uint; rep_override_en || scratch_uint--)
					{
						MEM_OP(extra ? REGS_BASE : SEGREG(scratch2_uint, REG_SI,), -, SEGREG(REG_ES, REG_DI,)),
						extra || INDEX_INC(REG_SI),
						INDEX_INC(REG_DI), rep_override_en && !(--regs16[REG_CX] && (!op_result == rep_mode)) && (scratch_uint = 0);
					}

					set_flags_type = FLAGS_UPDATE_SZP | FLAGS_UPDATE_AO_ARITH; // Funge to set SZP/AO flags
					set_CF(op_result > op_dest);
				}
			OPCODE 19: // RET|RETF|IRET
				i_d = i_w;
				R_M_POP(reg_ip);
				if (extra) // IRET|RETF|RETF imm16
					R_M_POP(regs16[REG_CS]);
				if (extra & 2) // IRET
					set_flags(R_M_POP(scratch_uint));
				else if (!i_d) // RET|RETF imm16
					regs16[REG_SP] += i_data0
			OPCODE 20: // MOV r/m, immed
				R_M_OP(mem[op_from_addr], =, i_data2)
			OPCODE 21: // IN AL/AX, DX/imm8
				io_ports[0x20] = 0; // PIC EOI
				io_ports[0x42] = --io_ports[0x40]; // PIT channel 0/2 read placeholder
				io_ports[0x3DA] ^= 9; // CGA refresh
				scratch_uint = extra ? regs16[REG_DX] : (unsigned char)i_data0;
				scratch_uint == 0x60 && (io_ports[0x64] = 0); // Scancode read flag
				scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 7) && (io_ports[0x3D5] = ((mem[0x49E]*80 + mem[0x49D] + CAST(short)mem[0x4AD]) & (io_ports[0x3D4] & 1 ? 0xFF : 0xFF00)) >> (io_ports[0x3D4] & 1 ? 0 : 8)); // CRT cursor position
				R_M_OP(regs8[REG_AL], =, io_ports[scratch_uint]);
			OPCODE 22: // OUT DX/imm8, AL/AX
				scratch_uint = extra ? regs16[REG_DX] : (unsigned char)i_data0;
				R_M_OP(io_ports[scratch_uint], =, regs8[REG_AL]);
				scratch_uint == 0x61 && (io_hi_lo = 0, spkr_en |= regs8[REG_AL] & 3); // Speaker control
				(scratch_uint == 0x40 || scratch_uint == 0x42) && (io_ports[0x43] & 6) && (mem[0x469 + scratch_uint - (io_hi_lo ^= 1)] = regs8[REG_AL]); // PIT rate programming
#ifndef NO_AUDIO
#ifndef NO_GRAPHICS
				scratch_uint == 0x43 && (io_hi_lo = 0, regs8[REG_AL] >> 6 == 2) && (SDL_PauseAudio((regs8[REG_AL] & 0xF7) != 0xB6), 0); // Speaker enable
#endif
#endif
				scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 6) && (mem[0x4AD + !(io_ports[0x3D4] & 1)] = regs8[REG_AL]); // CRT video RAM start offset
				scratch_uint == 0x3D5 && (io_ports[0x3D4] >> 1 == 7) && (scratch2_uint = ((mem[0x49E]*80 + mem[0x49D] + CAST(short)mem[0x4AD]) & (io_ports[0x3D4] & 1 ? 0xFF00 : 0xFF)) + (regs8[REG_AL] << (io_ports[0x3D4] & 1 ? 0 : 8)) - CAST(short)mem[0x4AD], mem[0x49D] = scratch2_uint % 80, mem[0x49E] = scratch2_uint / 80); // CRT cursor position
				scratch_uint == 0x3B5 && io_ports[0x3B4] == 1 && (GRAPHICS_X = regs8[REG_AL] * 16); // Hercules resolution reprogramming. Defaults are set in the BIOS
				scratch_uint == 0x3B5 && io_ports[0x3B4] == 6 && (GRAPHICS_Y = regs8[REG_AL] * 4);
			OPCODE 23: // REPxx
				rep_override_en = 2;
				rep_mode = i_w;
				seg_override_en && seg_override_en++
			OPCODE 25: // PUSH reg
				R_M_PUSH(regs16[extra])
			OPCODE 26: // POP reg
				R_M_POP(regs16[extra])
			OPCODE 27: // xS: segment overrides
				seg_override_en = 2;
				seg_override = extra;
				rep_override_en && rep_override_en++
			OPCODE 28: // DAA/DAS
				i_w = 0;
				extra ? DAA_DAS(-=, >=, 0xFF, 0x99) : DAA_DAS(+=, <, 0xF0, 0x90) // extra = 0 for DAA, 1 for DAS
			OPCODE 29: // AAA/AAS
				op_result = AAA_AAS(extra - 1)
			OPCODE 30: // CBW
				regs8[REG_AH] = -SIGN_OF(regs8[REG_AL])
			OPCODE 31: // CWD
				regs16[REG_DX] = -SIGN_OF(regs16[REG_AX])
			OPCODE 32: // CALL FAR imm16:imm16
				R_M_PUSH(regs16[REG_CS]);
				R_M_PUSH(reg_ip + 5);
				regs16[REG_CS] = i_data2;
				reg_ip = i_data0
			OPCODE 33: // PUSHF
				make_flags();
				R_M_PUSH(scratch_uint)
			OPCODE 34: // POPF
				set_flags(R_M_POP(scratch_uint))
			OPCODE 35: // SAHF
				make_flags();
				set_flags((scratch_uint & 0xFF00) + regs8[REG_AH])
			OPCODE 36: // LAHF
				make_flags(),
				regs8[REG_AH] = scratch_uint
			OPCODE 37: // LES|LDS reg, r/m
				i_w = i_d = 1;
				DECODE_RM_REG;
				OP(=);
				MEM_OP(REGS_BASE + extra, =, rm_addr + 2)
			OPCODE 38: // INT 3
				++reg_ip;
				pc_interrupt(3)
			OPCODE 39: // INT imm8
				reg_ip += 2;
#ifndef NO_GRAPHICS
				// Intercept INT 33h (mouse) before normal interrupt processing
				if (i_data0 == 0x33) {
					emu_mouse_poll_from_host();
					emulate_int33();
				} else
#endif
				pc_interrupt(i_data0)
			OPCODE 40: // INTO
				++reg_ip;
				regs8[FLAG_OF] && pc_interrupt(4)
			OPCODE 41: // AAM
				if (i_data0 &= 0xFF)
					regs8[REG_AH] = regs8[REG_AL] / i_data0,
					op_result = regs8[REG_AL] %= i_data0;
				else // Divide by zero
					pc_interrupt(0)
			OPCODE 42: // AAD
				i_w = 0;
				regs16[REG_AX] = op_result = 0xFF & regs8[REG_AL] + i_data0 * regs8[REG_AH]
			OPCODE 43: // SALC
				regs8[REG_AL] = -regs8[FLAG_CF]
			OPCODE 44: // XLAT
				regs8[REG_AL] = mem[SEGREG(seg_override_en ? seg_override : REG_DS, REG_BX, regs8[REG_AL] +)]
			OPCODE 45: // CMC
				regs8[FLAG_CF] ^= 1
			OPCODE 46: // CLC|STC|CLI|STI|CLD|STD
				regs8[extra / 2] = extra & 1
			OPCODE 47: // TEST AL/AX, immed
				R_M_OP(regs8[REG_AL], &, i_data0)
			OPCODE 48: // Emulator-specific 0F xx opcodes

				switch ((char)i_data0)
				{

					OPCODE_CHAIN 0: // PUTCHAR_AL
						//write(1, regs8, 1);
#ifdef USE_TMT
                        tmt_write(vterm, regs8, 1);
                        vterm_needs_draw = 1;
#else
                        putchar(*regs8);
                        fflush(stdout);
#endif
					OPCODE 1: // GET_RTC
						time(&clock_buf);
						ftime(&ms_clock);
						memcpy(mem + SEGREG(REG_ES, REG_BX,), localtime(&clock_buf), sizeof(struct tm));
						CAST(short)mem[SEGREG(REG_ES, REG_BX, 36+)] = ms_clock.millitm;
					OPCODE 2: // DISK_READ
                        OPCODE_CHAIN 3 : // DISK_WRITE
                            regs8[REG_AL] = ~fseek(disk[regs8[REG_DL]], CAST(unsigned)regs16[REG_BP] << 9, 0)
                            ? ((char)i_data0 == 3 ? fwrite(mem + SEGREG(REG_ES, REG_BX, ), 1, regs16[REG_AX], disk[regs8[REG_DL]]) : fread(mem + SEGREG(REG_ES, REG_BX, ), 1, regs16[REG_AX], disk[regs8[REG_DL]]))
                            : 0;


                    //OPCODE_CHAIN 3: // DISK_WRITE
                    //	regs8[REG_AL] = ~fseek(disk[regs8[REG_DL]], CAST(unsigned)regs16[REG_BP] << 9, 0)
                    //		? ((char)i_data0 == 3 ? (int(*)())fwrite : (int(*)())fread)(mem + SEGREG(REG_ES, REG_BX,), 1, regs16[REG_AX], disk[regs8[REG_DL]])
                        //	: 0;

				}

		}

		// Increment instruction pointer by computed instruction length. Tables in the BIOS binary
		// help us here.
		reg_ip += (i_mod*(i_mod != 3) + 2*(!i_mod && i_rm == 6))*i_mod_size + bios_table_lookup[TABLE_BASE_INST_SIZE][raw_opcode_id] + bios_table_lookup[TABLE_I_W_SIZE][raw_opcode_id]*(i_w + 1);

		// If instruction needs to update SF, ZF and PF, set them as appropriate
		if (set_flags_type & FLAGS_UPDATE_SZP)
		{
			regs8[FLAG_SF] = SIGN_OF(op_result);
			regs8[FLAG_ZF] = !op_result;
			regs8[FLAG_PF] = bios_table_lookup[TABLE_PARITY_FLAG][(unsigned char)op_result];

			// If instruction is an arithmetic or logic operation, also set AF/OF/CF as appropriate.
			if (set_flags_type & FLAGS_UPDATE_AO_ARITH)
				set_AF_OF_arith();
			if (set_flags_type & FLAGS_UPDATE_OC_LOGIC)
				set_CF(0), set_OF(0);
		}

		// Poll timer/keyboard every KEYBOARD_TIMER_UPDATE_DELAY instructions
		if (!(++inst_counter % KEYBOARD_TIMER_UPDATE_DELAY))
			int8_asap = 1;

		// Application has set trap flag, so fire INT 1
		if (trap_flag)
			pc_interrupt(1);

		trap_flag = regs8[FLAG_TF];

		// If a timer tick is pending, interrupts are enabled, and no overrides/REP are active,
		// then process the tick and check for new keystrokes
        if (int8_asap && !seg_override_en && !rep_override_en && regs8[FLAG_IF] && !regs8[FLAG_TF])
         {
             pc_interrupt(0xA);
             int8_asap = 0;

 #ifndef NO_GRAPHICS
             emu_mouse_poll_from_host();

 #ifdef QT_PORT
             /* Qt keyboard events are handled via the QWidget class.
                The emulator reads from a shared key queue. */
             /* Keys are injected by the Qt widget into mem[0x4A6] via pc_interrupt(7) */
 #else
             while(SDL_PollEvent(&sdl_event))
                 if(sdl_event.type == SDL_KEYDOWN || sdl_event.type == SDL_KEYUP)
                 {
                     scratch_uint = sdl_event.key.keysym.unicode;
                     scratch2_uint = sdl_event.key.keysym.mod;
                     scratch3_uint = sdl_key_to_ascii(sdl_event.key.keysym.sym);
                     CAST(short)mem[0x4A6] = 0x400 + 0x800*!!(scratch2_uint & KMOD_ALT) + 0x1000*!!(scratch2_uint & KMOD_SHIFT) + 0x2000*!!(scratch2_uint & KMOD_CTRL) + 0x4000*(sdl_event.type == SDL_KEYUP) + ((!(scratch_uint) || scratch_uint > 0x7F) ? scratch3_uint : scratch_uint);
                     pc_interrupt(7);
                 }
                 else if(sdl_event.type == SDL_MOUSEMOTION || sdl_event.type == SDL_MOUSEBUTTONDOWN || sdl_event.type == SDL_MOUSEBUTTONUP)
                 {
                     /* Mouse events handled by emu_mouse_poll_from_host above */
                 }
 #endif
 #else
             KEYBOARD_DRIVER;
 #endif
         }
	}
    // Draw virtual terminal
#ifdef USE_TMT
    if(vterm_needs_draw)
    {
        TMTSCREEN const *s = tmt_screen(vterm);
        for(size_t i = 0; i < VTERM_BLANK_LINES; i++)
            putchar('\n');
        for(size_t r = 0; r < s->nline; ++r)
        {
            for(size_t c = 0; c < s->ncol; ++c)
                if(!(s->lines[r]->chars[c].c & 0x80)) putchar(s->lines[r]->chars[c].c);
            putchar('\n');
#ifdef VTERM_USE_CR
            putchar('\r');
#endif
        }
        vterm_needs_draw = 0;
        tmt_clean(vterm);
    }
#endif

#ifndef NO_GRAPHICS
#ifndef REFRESH_DISPLAY_EVERY_FRAME
    // Update the video graphics display every GRAPHICS_UPDATE_DELAY instructions
    if (!(inst_counter % GRAPHICS_UPDATE_DELAY))
    {
#endif
    /* Video card in graphics mode? */
    if (io_ports[0x3B8] & 2)
    {
        if (is_display_init != 1)
        {
            is_display_init = 1;
            for (int i = 0; i < 16; i++)
                pixel_colors[i] = mem[0x4AC] ?
                    cga_colors[(i & 12) >> 2] + (cga_colors[i & 3] << 16)
                    : 0xFF*(((i & 1) << 24) + ((i & 2) << 15) + ((i & 4) << 6) + ((i & 8) >> 3));

            for (int i = 0; i < GRAPHICS_X * GRAPHICS_Y / 4; i++)
                vid_addr_lookup[i] = i / GRAPHICS_X * (GRAPHICS_X / 8) + (i / 2) % (GRAPHICS_X / 8) + 0x2000*(mem[0x4AC] ? (2 * i / GRAPHICS_X) % 2 : (4 * i / GRAPHICS_X) % 4);

#ifdef QT_PORT
            set_video_mode();
#else
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            set_video_mode();
#endif
            emu_mouse.guest_w = GRAPHICS_X;
            emu_mouse.guest_h = GRAPHICS_Y;
        }

        /* Refresh display from emulated video RAM */
        vid_mem_base = mem + 0xB0000 + 0x8000*(mem[0x4AC] ? 1 : io_ports[0x3B8] >> 7);

#ifdef QT_PORT
        qt_image_mutex.lock();
        uint32_t *bits = (uint32_t*)qt_image.bits();
        for (int i = 0; i < GRAPHICS_X * GRAPHICS_Y / 4; i++)
        {
            unsigned int col_4_pack = pixel_colors[15 & (vid_mem_base[vid_addr_lookup[i]] >> 4*!(i & 1))];
            int base_idx = i * 4;
            for(int j = 0; j < 4; ++j)
            {
                uchar r = col_4_pack & 0xE0;
                uchar g = (col_4_pack & 0x1C) << 3;
                uchar b = (col_4_pack & 0x03) << 6;
                bits[base_idx + j] = 0xFF000000 | (r << 16) | (g << 8) | b;
                col_4_pack >>= 8;
            }
        }
        qt_image_mutex.unlock();
        qt_screen_dirty = true;
#else
        if(SDL_MUSTLOCK(sdl_screen)) SDL_LockSurface(sdl_screen);
        for (int i = 0; i < GRAPHICS_X * GRAPHICS_Y / 4; i++)
        {
            unsigned int col_4_pack = pixel_colors[15 & (vid_mem_base[vid_addr_lookup[i]] >> 4*!(i & 1))];
            int x = (i % (GRAPHICS_X / 4)) * 4;
            int y = i / (GRAPHICS_X / 4);
            for(int j = 0; j < 4; ++j)
            {
                *((uint32_t*)sdl_screen->pixels + x + j + (y * GRAPHICS_X)) = SDL_MapRGBA(sdl_fmt, col_4_pack & 0xE0, (col_4_pack & 0x1C) << 3, (col_4_pack & 0x02) << 6, 255);
                col_4_pack >>= 8;
            }
        }
        if(SDL_MUSTLOCK(sdl_screen)) SDL_UnlockSurface(sdl_screen);
        SDL_Flip(sdl_screen);
#endif
    }
    else
    {
        if (is_display_init == 1)
        {
#ifdef QT_PORT
            GRAPHICS_X = GRAPHICS_X_DEFAULT;
            GRAPHICS_Y = GRAPHICS_Y_DEFAULT;
            set_video_mode();
#else
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            GRAPHICS_X = GRAPHICS_X_DEFAULT;
            GRAPHICS_Y = GRAPHICS_Y_DEFAULT;
            set_video_mode();
#endif
        }
    }
        //SDL_PumpEvents();
#ifndef REFRESH_DISPLAY_EVERY_FRAME
    }
#endif
#ifndef PUMP_EVENTS_EVERY_FRAME
  //  if (!(inst_counter % GRAPHICS_UPDATE_DELAY))
#endif
        //SDL_PumpEvents();
#endif
}

#ifdef QT_PORT
/* ============================================================
   QT 5.12 WIDGET — Replaces the SDL window entirely
   ============================================================ */
class EmulatorWidget : public QWidget {
public:
    EmulatorWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle("8086tiny Qt 5.12 — allycat.exe");
        resize(GRAPHICS_X_DEFAULT, GRAPHICS_Y_DEFAULT);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        qt_widget = this;

        /* CPU tick timer — replaces the while(cont_main_loop) loop */
        cpuTimer = new QTimer(this);
        connect(cpuTimer, &QTimer::timeout, this, &EmulatorWidget::cpu_tick);
        cpuTimer->start(16); /* ~60 FPS */
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (qt_screen_dirty) {
            QMutexLocker locker(&qt_image_mutex);
            /* Scale to fit widget if resized */
            painter.drawImage(0, 0, qt_image.scaled(size(), Qt::IgnoreAspectRatio, Qt::FastTransformation));
        }
    }

    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        emu_mouse.host_w = width();
        emu_mouse.host_h = height();
    }

    void keyPressEvent(QKeyEvent *event) override {
        inject_key(event, true);
    }

    void keyReleaseEvent(QKeyEvent *event) override {
        inject_key(event, false);
    }

    void closeEvent(QCloseEvent*) override {
        cont_main_loop = 0;
    }

private:
    QTimer *cpuTimer;

    void inject_key(QKeyEvent *event, bool is_down) {
        /* Translate Qt key event to 8086tiny BIOS keyboard format */
        unsigned int unicode = 0;
        QString text = event->text();
        if (!text.isEmpty()) unicode = text.at(0).unicode();

        unsigned int mod = 0;
        if (event->modifiers() & Qt::AltModifier)   mod |= 0x800;
        if (event->modifiers() & Qt::ShiftModifier)  mod |= 0x1000;
        if (event->modifiers() & Qt::ControlModifier) mod |= 0x2000;

        unsigned int ascii = qt_key_to_ascii(event->key());

        /* Write to BIOS keyboard buffer and fire INT 9 (keyboard interrupt) */
        CAST(short)mem[0x4A6] = 0x400 + mod + 0x4000*(!is_down) +
            ((!(unicode) || unicode > 0x7F) ? ascii : unicode);
        pc_interrupt(7);
    }

    void cpu_tick() {
        if (cont_main_loop) {
            /* Execute one batch of instructions per frame */
            main_loop();
            if (qt_screen_dirty) {
                update();
                qt_screen_dirty = false;
            }
        }
    }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    /* Initialize the emulator: loads bios.rom + fd.img */
    init(argc, argv);

    EmulatorWidget w;
    w.show();

    int result = app.exec();

    /* Cleanup */
    for (int i = 0; i < 2; i++)
        if (disk[i]) fclose(disk[i]);

    return result;
}

#else
/* ============================================================
   ORIGINAL SDL main() — kept for non-Qt builds
   ============================================================ */
int main(int argc, char** argv)
{
    init(argc, argv);
    while(cont_main_loop == 1) main_loop();
    quit();
    return 0;
}
#endif
