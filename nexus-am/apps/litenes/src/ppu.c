#include <ppu.h>
#include <cpu.h>
#include <fce.h>
#include <memory.h>
#include <klib.h>
#include <stdint.h>

static const word ppu_base_nametable_addresses[4] = { 0x2000, 0x2400, 0x2800, 0x2C00 };

byte PPU_SPRRAM[0x100];
byte PPU_RAM[0x4000];
byte ppu_sprite_palette[4][4];
bool ppu_2007_first_read;
byte ppu_addr_latch;
PPU_STATE ppu;
byte ppu_latch;
bool ppu_sprite_hit_occured = false;
byte ppu_screen_background[264][264];


// preprocess tables
static byte XHL[8][256][256];
static uint64_t XHL64[256][256];
static uint64_t XHLmask[256][256];
static word ppu_ram_map[0x4000];

static inline void draw(int col, int row, int idx) {
  canvas[row][col + 0xff] = idx;
  if(idx != 0) {
	  log("row:%d, col:%d, idx:%d\n", row, col, idx);
  }
}

// PPUCTRL Functions

inline word ppu_base_nametable_address()                            { return ppu_base_nametable_addresses[ppu.PPUCTRL & 0x3];  }
inline byte ppu_vram_address_increment()                            { return common_bit_set(ppu.PPUCTRL, 2) ? 32 : 1;          }
inline word ppu_sprite_pattern_table_address()                      { return common_bit_set(ppu.PPUCTRL, 3) ? 0x1000 : 0x0000; }
inline word ppu_background_pattern_table_address()                  { return common_bit_set(ppu.PPUCTRL, 4) ? 0x1000 : 0x0000; }
inline byte ppu_sprite_height()                                     { return common_bit_set(ppu.PPUCTRL, 5) ? 16 : 8;          }
inline bool ppu_generates_nmi()                                     { return common_bit_set(ppu.PPUCTRL, 7);                   }



// PPUMASK Functions

inline bool ppu_renders_grayscale()                                 { return common_bit_set(ppu.PPUMASK, 0); }
inline bool ppu_shows_background_in_leftmost_8px()                  { return common_bit_set(ppu.PPUMASK, 1); }
inline bool ppu_shows_sprites_in_leftmost_8px()                     { return common_bit_set(ppu.PPUMASK, 2); }
inline bool ppu_shows_background()                                  { return common_bit_set(ppu.PPUMASK, 3); }
inline bool ppu_shows_sprites()                                     { return common_bit_set(ppu.PPUMASK, 4); }
inline bool ppu_intensifies_reds()                                  { return common_bit_set(ppu.PPUMASK, 5); }
inline bool ppu_intensifies_greens()                                { return common_bit_set(ppu.PPUMASK, 6); }
inline bool ppu_intensifies_blues()                                 { return common_bit_set(ppu.PPUMASK, 7); }

inline void ppu_set_renders_grayscale(bool yesno)                   { common_modify_bitb(&ppu.PPUMASK, 0, yesno); }
inline void ppu_set_shows_background_in_leftmost_8px(bool yesno)    { common_modify_bitb(&ppu.PPUMASK, 1, yesno); }
inline void ppu_set_shows_sprites_in_leftmost_8px(bool yesno)       { common_modify_bitb(&ppu.PPUMASK, 2, yesno); }
inline void ppu_set_shows_background(bool yesno)                    { common_modify_bitb(&ppu.PPUMASK, 3, yesno); }
inline void ppu_set_shows_sprites(bool yesno)                       { common_modify_bitb(&ppu.PPUMASK, 4, yesno); }
inline void ppu_set_intensifies_reds(bool yesno)                    { common_modify_bitb(&ppu.PPUMASK, 5, yesno); }
inline void ppu_set_intensifies_greens(bool yesno)                  { common_modify_bitb(&ppu.PPUMASK, 6, yesno); }
inline void ppu_set_intensifies_blues(bool yesno)                   { common_modify_bitb(&ppu.PPUMASK, 7, yesno); }



// PPUSTATUS Functions

inline bool ppu_sprite_overflow()                                   { return common_bit_set(ppu.PPUSTATUS, 5); }
inline bool ppu_sprite_0_hit()                                      { return common_bit_set(ppu.PPUSTATUS, 6); }
inline bool ppu_in_vblank()                                         { return common_bit_set(ppu.PPUSTATUS, 7); }

inline void ppu_set_sprite_overflow(bool yesno)                     { common_modify_bitb(&ppu.PPUSTATUS, 5, yesno); }
inline void ppu_set_sprite_0_hit(bool yesno)                        { common_modify_bitb(&ppu.PPUSTATUS, 6, yesno); }
inline void ppu_set_in_vblank(bool yesno)                           { common_modify_bitb(&ppu.PPUSTATUS, 7, yesno); }


// RAM


inline word ppu_get_real_ram_address(word address)
{
    if (address < 0x2000) {
        return address;
    }
    else if (address < 0x3F00) {
        if (address < 0x3000) {
            return address;
        }
        else {
            return address;// - 0x1000;
        }
    }
    else if (address < 0x4000) {
        address = 0x3F00 | (address & 0x1F);
        if (address == 0x3F10 || address == 0x3F14 || address == 0x3F18 || address == 0x3F1C)
            return address - 0x10;
        else
            return address;
    }
    return 0xFFFF;
}

inline byte ppu_ram_read(word address)
{
	log("ppu_ram_read: address:%x, map:%x\n", address, ppu_ram_map[address]);
    return PPU_RAM[ppu_ram_map[address]];
}

inline void ppu_ram_write(word address, byte data)
{
    PPU_RAM[ppu_ram_map[address]] = data;
}

// 3F01 = 0F (00001111)
// 3F02 = 2A (00101010)
// 3F03 = 09 (00001001)
// 3F04 = 07 (00000111)
// 3F05 = 0F (00001111)
// 3F06 = 30 (00110000)
// 3F07 = 27 (00100111)
// 3F08 = 15 (00010101)
// 3F09 = 0F (00001111)
// 3F0A = 30 (00110000)
// 3F0B = 02 (00000010)
// 3F0C = 21 (00100001)
// 3F0D = 0F (00001111)
// 3F0E = 30 (00110000)
// 3F0F = 00 (00000000)
// 3F11 = 0F (00001111)
// 3F12 = 16 (00010110)
// 3F13 = 12 (00010010)
// 3F14 = 37 (00110111)
// 3F15 = 0F (00001111)
// 3F16 = 12 (00010010)
// 3F17 = 16 (00010110)
// 3F18 = 37 (00110111)
// 3F19 = 0F (00001111)
// 3F1A = 17 (00010111)
// 3F1B = 11 (00010001)
// 3F1C = 35 (00110101)
// 3F1D = 0F (00001111)
// 3F1E = 17 (00010111)
// 3F1F = 11 (00010001)
// 3F20 = 2B (00101011)

static inline uint64_t int_shl(int n, int s) {
  // compute (uint64_t)n << s without generating shld instruction in x86
  int hi, lo;
  if (s < 32) {
    lo = n << s;
    hi = n >> (32 - s);
  }
  else {
    lo = 0;
    hi = n << (s - 32);
  }
  return ((uint64_t)hi << 32) | lo;
}

// Rendering
static void table_init() {
  for (int x = 0; x < 8; x ++) {
    for (int h = 0; h < 256; h ++)
      for (int l = 0; l < 256; l ++) {
        int col = (((h >> (7 - x)) & 1) << 1) | ((l >> (7 - x)) & 1);
        XHL[x][h][l] = col;
        XHL64[h][l] |= int_shl(col, x * 8);
        if (col == 0) {
          XHLmask[h][l] |= int_shl(0xff,x * 8);
        }
      }
  }

  for (int x = 0; x < 0x4000; x ++) {
    ppu_ram_map[x] = ppu_get_real_ram_address(x);
  }
  log("ppu_ram_map[0x3F00]=0x%x\n", ppu_ram_map[0x3F00]);
}

void ppu_draw_background_scanline(bool mirror)
{
	log("draw_background_scanline called\n");
    int tile_x, tile_y = ppu.scanline >> 3;
    int taddr = ppu_base_nametable_address() + (tile_y << 5) + (mirror ? 0x400 : 0);
    int y_in_tile = ppu.scanline & 0x7;
    int scroll_base = - ppu.PPUSCROLL_X + (mirror ? 256 : 0);

    int palette_cache[4], do_update = frame_cnt % 3 == 0;

    word attribute_address = (ppu_base_nametable_address() + (mirror ? 0x400 : 0) + 0x3C0 +  -1 + ((ppu.scanline >> 5) << 3));

    for (tile_x = ppu_shows_background_in_leftmost_8px() ? 0 : 1; tile_x < 32; tile_x++) {
        // Skipping off-screen pixels
        if (((tile_x << 3) - ppu.PPUSCROLL_X + (mirror ? 256 : 0)) > 256)
            continue;

		log("call ppu_ram_read\n");
        int tile_index = ppu_ram_read(taddr);
        word tile_address = ppu_background_pattern_table_address() + (tile_index << 4);

        byte l = ppu_ram_read(tile_address + y_in_tile);
        byte h = ppu_ram_read(tile_address + y_in_tile + 8);

        if (do_update) {
            attribute_address += (tile_x & 3) == 0;
            bool top = (ppu.scanline & 31) < 16;
            bool left = ((tile_x & 31) < 16);
			log("call ppu_ram_read\n");
            byte palette_attribute = ppu_ram_read(attribute_address);
            if (!top) {
                palette_attribute >>= 4;
            }
            if (!left) {
                palette_attribute >>= 2;
            }
            palette_attribute &= 3;

            word palette_address = 0x3F00 + (palette_attribute << 2);
			log("call ppu_ram_read\n");
            palette_cache[1] = ppu_ram_read(palette_address + 1);
            palette_cache[2] = ppu_ram_read(palette_address + 2);
            palette_cache[3] = ppu_ram_read(palette_address + 3);
            for (int x = 0; x < 8; x ++) {
                byte color = XHL[x][h][l];
                // lookup-table is much faster on x86.
                // byte color = (((h >> (7 - x)) & 1) << 1) | ((l >> (7 - x)) & 1);
                if (color != 0) {
                    draw(scroll_base + x, ppu.scanline + 1, palette_cache[color]);
                }
            }
        }
        uint64_t *ptr = (uint64_t*)&ppu_screen_background[ppu.scanline][(tile_x << 3)];
        *ptr = (XHL64[h][l]) | (XHLmask[h][l] & (*ptr)) ;

        taddr ++;
        scroll_base += 8;
    }
}

void ppu_draw_sprite_scanline()
{
	log("====\n");
    int do_update = frame_cnt % 3 == 0;
    int scanline_sprite_count = 0;
    int n;
    for (n = 0; n < 0x100; n += 4) {
        byte sprite_x = PPU_SPRRAM[n + 3];
        byte sprite_y = PPU_SPRRAM[n];

        // Skip if sprite not on scanline
        if (sprite_y > ppu.scanline || sprite_y + ppu_sprite_height() < ppu.scanline)
           continue;

        scanline_sprite_count++;

        // PPU can't render > 8 sprites
        if (scanline_sprite_count > 8) {
            ppu_set_sprite_overflow(true);
            // break;
        }

        bool vflip = PPU_SPRRAM[n + 2] & 0x80;
        bool hflip = PPU_SPRRAM[n + 2] & 0x40;

        word tile_address = ppu_sprite_pattern_table_address() + 16 * PPU_SPRRAM[n + 1];
        int y_in_tile = ppu.scanline & 0x7;
		log("call ppu_ram_read\n");
        byte l = ppu_ram_read(tile_address + (vflip ? (7 - y_in_tile) : y_in_tile));
		log("call ppu_ram_read\n");
        byte h = ppu_ram_read(tile_address + (vflip ? (7 - y_in_tile) : y_in_tile) + 8);

        byte palette_attribute = PPU_SPRRAM[n + 2] & 0x3;
        word palette_address = 0x3F10 + (palette_attribute << 2);
        int x;
        for (x = 0; x < 8; x++) {
            int color = hflip ? XHL[7 - x][h][l] : XHL[x][h][l];

            // Color 0 is transparent
            if (color != 0) {
                int screen_x = sprite_x + x;

                if (do_update) {
					log("call ppu_ram_read\n");
                    int idx = ppu_ram_read(palette_address + color);
										printf("%d ",idx);
                    if (PPU_SPRRAM[n + 2] & 0x20) {
                        draw(screen_x, sprite_y + y_in_tile + 1, idx); // bbg
                    }
                    else {
                        draw(screen_x, sprite_y + y_in_tile + 1, idx); // fg
                    }
                }

                // Checking sprite 0 hit
                if (ppu_shows_background() && !ppu_sprite_hit_occured && n == 0 && ppu_screen_background[sprite_y + y_in_tile][screen_x] == color) {
                    ppu_set_sprite_0_hit(true);
                    ppu_sprite_hit_occured = true;
                }
            }
        }
    }
}



// PPU Lifecycle

void ppu_run(int cycles)
{
	log("cycles:%d\n", cycles);
    while (cycles-- > 0) {
        ppu_cycle();
    }
	log("cycles:%d\n", cycles);
}

void ppu_cycle()
{
    if (!ppu.ready && cpu_clock() > 29658)
        ppu.ready = true;

    ppu.scanline++;
	log("flag:%x, %x\n", ppu_shows_background(), ppu_shows_sprites());
    if (ppu_shows_background()) {
		log("shows_background\n");
        ppu_draw_background_scanline(false);
        ppu_draw_background_scanline(true);
    }
    
    if (ppu_shows_sprites()) {
		log("ppu_draw_sprite_scanline\n");
		ppu_draw_sprite_scanline();
	}

    if (ppu.scanline == 241) {
        ppu_set_in_vblank(true);
        ppu_set_sprite_0_hit(false);
        cpu_interrupt();
    }
    else if (ppu.scanline == 262) {
        ppu.scanline = -1;
        ppu_sprite_hit_occured = false;
        ppu_set_in_vblank(false);
		log("fce_update_screen\n");
        fce_update_screen();
    }
}

inline void ppu_copy(word address, byte *source, int length)
{
    memcpy(&PPU_RAM[address], source, length);
}

inline byte ppu_io_read(word address)
{
    ppu.PPUADDR &= 0x3FFF;
    switch (address & 7) {
        case 2:
        {
            byte value = ppu.PPUSTATUS;
            ppu_set_in_vblank(false);
            ppu_set_sprite_0_hit(false);
            ppu.scroll_received_x = 0;
            ppu.PPUSCROLL = 0;
            ppu.addr_received_high_byte = 0;
            ppu_latch = value;
            ppu_addr_latch = 0;
            ppu_2007_first_read = true;
            return value;
        }
        case 4: return ppu_latch = PPU_SPRRAM[ppu.OAMADDR];
        case 7:
        {
            byte data;
            
            if (ppu.PPUADDR < 0x3F00) {
                data = ppu_latch = ppu_ram_read(ppu.PPUADDR);
            }
            else {
                data = ppu_ram_read(ppu.PPUADDR);
                ppu_latch = 0;
            }
            
            if (ppu_2007_first_read) {
                ppu_2007_first_read = false;
            }
            else {
                ppu.PPUADDR += ppu_vram_address_increment();
            }
            return data;
        }
        default:
            return 0xFF;
    }
}

inline void ppu_io_write(word address, byte data)
{
    address &= 7;
    ppu_latch = data;
    ppu.PPUADDR &= 0x3FFF;
    switch(address) {
        case 0: if (ppu.ready) ppu.PPUCTRL = data; break;
        case 1: if (ppu.ready) ppu.PPUMASK = data; break;
        case 3: ppu.OAMADDR = data; break;
        case 4: PPU_SPRRAM[ppu.OAMADDR++] = data; break;
        case 5:
        {
            if (ppu.scroll_received_x)
                ppu.PPUSCROLL_Y = data;
            else
                ppu.PPUSCROLL_X = data;

            ppu.scroll_received_x ^= 1;
            break;
        }
        case 6:
        {
            if (!ppu.ready)
                return;

            if (ppu.addr_received_high_byte)
                ppu.PPUADDR = (ppu_addr_latch << 8) + data;
            else
                ppu_addr_latch = data;

            ppu.addr_received_high_byte ^= 1;
            ppu_2007_first_read = true;
            break;
        }
        case 7:
        {
            if (ppu.PPUADDR > 0x1FFF || ppu.PPUADDR < 0x4000) {
				log("ppu_write.1(%x,%d)\n", ppu.PPUADDR ^ ppu.mirroring_xor, data);
                ppu_ram_write(ppu.PPUADDR ^ ppu.mirroring_xor, data);
				log("ppu_write.2(%x,%d)\n", ppu.PPUADDR, data);
                ppu_ram_write(ppu.PPUADDR, data);
            }
            else {
				log("ppu_write.3(%x,%d)\n", ppu.PPUADDR, data);
                ppu_ram_write(ppu.PPUADDR, data);
            }
        }
    }
    ppu_latch = data;
}

void ppu_init()
{
    ppu.PPUCTRL = ppu.PPUMASK = ppu.PPUSTATUS = ppu.OAMADDR = ppu.PPUSCROLL_X = ppu.PPUSCROLL_Y = ppu.PPUADDR = 0;
    ppu.PPUSTATUS |= 0xA0;
    ppu.PPUDATA = 0;
    ppu_2007_first_read = true;
    table_init();
}

void ppu_sprram_write(byte data)
{
    PPU_SPRRAM[ppu.OAMADDR++] = data;
}

void ppu_set_background_color(byte color)
{
}

void ppu_set_mirroring(byte mirroring)
{
    ppu.mirroring = mirroring;
    ppu.mirroring_xor = 0x400 << mirroring;
}
