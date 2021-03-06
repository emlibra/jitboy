#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core.h"
#include "memory.h"

static uint8_t get_joypad_state(gb_keys *keys, uint8_t value)
{
    uint8_t result = 0;
    if (value & ~0x10)
        result |= keys->state & 0x0f;
    if (value & ~0x20)
        result |= (keys->state >> 4);
    return ~result;
}

/* change RAM bank to bank if supported */
static void gb_memory_change_ram_bank(gb_memory *mem, int bank)
{
    if (mem->current_ram_bank == bank)
        return;

    if (!mem->rtc_access) {
        memcpy(mem->ram_banks + mem->current_ram_bank * 0x2000,
               mem->mem + 0xa000, 0x2000);
    }
    memcpy(mem->mem + 0xa000, mem->ram_banks + bank * 0x2000, 0x2000);
    mem->rtc_access = false;

    mem->current_ram_bank = bank;
}

/* change ROM bank to bank if supported */
static void gb_memory_change_rom_bank(gb_memory *mem, int bank)
{
    if (mem->current_rom_bank == bank)
        return;

    if (munmap(mem->mem + 0x4000, 0x4000) != 0) {
        printf("munmap failed (%i)\n", errno);
        return;
    }

    if (mmap(mem->mem + 0x4000, 0x4000, PROT_READ, MAP_PRIVATE | MAP_FIXED,
             mem->fd, 0x4000 * bank) == MAP_FAILED) {
        printf("mmap failed! (%i)\n", errno);
        return;
    }

    mem->current_rom_bank = bank;
}

static void gb_memory_access_rtc(gb_memory *mem, int elt)
{
    /* FIXME: implement RTC */
    mem->mem[0xa000] = 0;
    mem->rtc_access = true;
}

static void gb_memory_update_rtc_time(gb_memory *mem, int value)
{
    /* FIXME: implement RTC time */
}

/* emulate write through mbc */
void gb_memory_write(gb_state *state, uint64_t addr, uint64_t value)
{
    addr &= 0xffff;
    value &= 0xff;

    uint8_t *mem = state->mem->mem;

    if (addr < 0x8000) {
        LOG_DEBUG("write to rom @address %#lx, value is %#lx\n", addr, value);

        switch (state->mem->mbc) {
        case MBC_NONE:
            break;
        case MBC2_BAT:
        case MBC2:
        case MBC1_RAM_BAT:
        case MBC1:
            if (addr >= 0x6000) {
                state->mem->mbc_mode = value & 0x01;
            } else if (addr >= 0x4000) {
                if (state->mem->mbc_mode) {
                    gb_memory_change_ram_bank(state->mem, value);
                } else {
                    state->mem->mbc_data = value << 5;
                }
            } else if (addr >= 0x2000) {
                int bank =
                    (value & 0x1f) |
                    (state->mem->mbc_mode ? 0 : (state->mem->mbc_data & 0x60));

                if ((bank & 0x1f) == 0)
                    bank |= 1;

                LOG_DEBUG("change rom bank to %i\n", bank);
                gb_memory_change_rom_bank(state->mem, bank);
            }
            break;
        case MBC3_TIMER_RAM_BAT:
        case MBC3_RAM_BAT:
        case MBC3:
            if (addr >= 0x6000) {
                gb_memory_update_rtc_time(state->mem, value);
            } else if (addr >= 0x4000) {
                if (value <= 4)
                    gb_memory_change_ram_bank(state->mem, value);
                else
                    gb_memory_access_rtc(state->mem, value);
            } else if (addr >= 0x2000) {
                int bank = (value & 0x7f);
                if (bank == 0)
                    bank = 1;

                LOG_DEBUG("change rom bank to %i\n", bank);
                gb_memory_change_rom_bank(state->mem, bank);
            }
            break;
        case MBC5_RAM_BAT:
        case MBC5:
            if (addr >= 0x4000) {
                gb_memory_change_ram_bank(state->mem, value);
            } else if (addr >= 0x2000) {
                int bank = value;

                LOG_DEBUG("change rom bank to %i\n", bank);
                gb_memory_change_rom_bank(state->mem, bank);
            }
            break;
        default:
            printf("Unknown MBC, cannot switch bank\n");
            break;
        }
    } else if (addr == 0xff05) {
        LOG_DEBUG("Memory write to %#lx, reset to 0\n", addr);
        mem[addr] = 0;
    } else if (addr == 0xff00) { /* check for keypresses */
        LOG_DEBUG("Reading joypad state @%4x\n", state->pc);
        mem[addr] = get_joypad_state(&state->keys, value);
    } else if (addr == 0xff01) {
        LOG_DEBUG("Writing serial transfer data @%4x\n", state->pc);
    } else if (addr == 0xff46) { /* DMA Transfer to OAM RAM */
                                 /* Detect jumps in the RAM and optimize DMA */
        LOG_DEBUG("DMA Transfer started.\n");
        mem[addr] = value;
        memcpy(&mem[0xfe00], &mem[value << 8], 0xa0);
    } else if (addr >= 0xff80) { /* write to internal ram */
        gb_vm *vm = (gb_vm *) state;

        /* invalidate compiled blocks */
        for (unsigned i = 0; i < addr - 0xff80; ++i) {
            if (vm->highmem_blocks[i].exec_count != 0 &&
                vm->highmem_blocks[i].end_address > addr) {
                free_block(&vm->highmem_blocks[i]);
                vm->highmem_blocks[i].exec_count = 0;
            }
        }

        mem[addr] = value;
    } else {
        LOG_DEBUG("Memory write to %#lx, value is %#lx\n", addr, value);
        mem[addr] = value;
    }
}

/* initialize memory layout and map file filename */
bool gb_memory_init(gb_memory *mem, const char *filename)
{
    if (!filename) {
        mem->fd = -1;
        mem->mem = mmap((void *) 0x1000000, 0x10000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (mem->mem == MAP_FAILED) {
            printf("Map failed! (%i)\n", errno);
            return false;
        }
    } else {
        mem->fd = open(filename, O_RDONLY);
        if (mem->fd < 0) {
            printf("Could not open file! (%i)\n", errno);
            return false;
        }

        mem->mem = mmap((void *) 0x1000000, 0x8000, PROT_READ, MAP_PRIVATE,
                        mem->fd, 0);
        if (mem->mem == MAP_FAILED) {
            printf("Map failed! (%i)\n", errno);
            return false;
        }

        if (mmap(mem->mem + 0x8000, 0x8000, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
                 0) == MAP_FAILED) {
            printf("Allocating memory failed! (%i)\n", errno);
            return false;
        }
    }

    mem->ram_banks = malloc(MAX_RAM_BANKS * 0x2000);

    mem->filename = filename;
    mem->mbc = mem->mem[0x0147];
    mem->mbc_mode = 0;
    mem->mbc_data = 0;
    mem->current_rom_bank = 1;
    mem->current_ram_bank = 0;
    mem->rtc_access = false;

    return true;
}

/* free memory again */
bool gb_memory_free(gb_memory *mem)
{
    free(mem->ram_banks);

    close(mem->fd);

    if (munmap(mem->mem, 0x8000) != 0 ||
        munmap(mem->mem + 0x8000, 0x8000) != 0) {
        printf("munmap failed (%i)\n", errno);
        return false;
    }
    return true;
}

void dump_header_info(gb_memory *mem)
{
    printf("ROM information about file %s:\n", mem->filename);
    printf("+ Title: %s\n", mem->mem + 0x134);
    printf("+ Manufacturer: %s\n", mem->mem + 0x13f);
    printf("+ Cartridge type: %#2x\n", mem->mem[0x147]);
    printf("+ ROM size: %i KiB\n", 32 << mem->mem[0x148]);
    printf("+ RAM size: %i KiB\n",
           mem->mem[0x149] > 0 ? 1 << (mem->mem[0x149] * 2 - 1) : 0);
    printf("\n");
}
