/****************************************************************************
 * D13x Bootloader - linker.ld
 *
 * D13x bootloader 链接脚本
 *
 ****************************************************************************/

/* 入口点 */

ENTRY(_start)

/* 内存布局 */

MEMORY
{
  sram (rwx) : ORIGIN = 0x20000000, LENGTH = 1M
  psram (rwx) : ORIGIN = 0x30000000, LENGTH = 8M
}

/* 段定义 */

SECTIONS
{
  /* 代码段 */

  .text : {
    _stext = .;
    *(.text.startup)
    *(.text .text.*)
    _etext = .;
  } > sram

  /* 只读数据段 */

  .rodata : {
    _srodata = .;
    *(.rodata .rodata.*)
    _erodata = .;
  } > sram

  /* 已初始化数据段 */

  .data : {
    _sdata = .;
    *(.data .data.*)
    _edata = .;
  } > sram AT > sram

  /* 未初始化数据段 */

  .bss : {
    _sbss = .;
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > sram

  /* 堆栈 */

  .stack (NOLOAD) : {
    . = ALIGN(8);
    . = . + 4096;
    _estack = .;
  } > sram
}
