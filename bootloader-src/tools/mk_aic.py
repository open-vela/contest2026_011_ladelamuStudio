#!/usr/bin/env python3
"""
D13x Bootloader .aic Image Creator

创建符合 ArtInChip BROM 规范的 .aic 启动镜像
"""

import struct
import sys
import os

def create_aic_image(bootloader_bin, output_file, load_addr=0x20000000, entry_addr=0x20000000):
    """
    创建 .aic 格式的启动镜像
    
    参数:
        bootloader_bin: bootloader 二进制文件路径
        output_file: 输出 .aic 文件路径
        load_addr: 加载地址
        entry_addr: 入口地址
    """
    
    # 读取 bootloader 数据
    with open(bootloader_bin, 'rb') as f:
        bootloader_data = f.read()
    
    # 计算填充（256字节对齐）
    padding_size = (256 - (len(bootloader_data) % 256)) % 256
    padded_data = bootloader_data + b'\x00' * padding_size
    
    # HEAD1 (8 bytes)
    magic = b'AIC '
    checksum = 0  # 非安全启动时为0
    head1 = magic + struct.pack('<I', checksum)
    
    # HEAD2 (248 bytes)
    header_version = 0x00010001
    image_length = 8 + 248 + len(padded_data) + 16 + 256  # head1 + head2 + data + iv + signature
    firmware_version = 1
    loader_length = len(bootloader_data)
    load_address = load_addr
    entry_point = entry_addr
    signature_algo = 0  # 无签名
    encryption_algo = 0  # 不加密
    sig_result_offset = 8 + 248 + len(padded_data)  # MD5 结果位置
    sig_result_length = 16  # MD5 16字节
    sig_key_offset = 0
    sig_key_length = 0
    iv_offset = sig_result_offset + sig_result_length
    iv_length = 16  # AES IV 16字节
    private_offset = iv_offset + iv_length
    private_length = 0
    pbp_offset = private_offset + private_length
    pbp_length = 0
    
    head2 = struct.pack('<IIIIIIIIIIIIIIIIIIIII',
        header_version, image_length, firmware_version, loader_length,
        load_address, entry_point, signature_algo, encryption_algo,
        sig_result_offset, sig_result_length, sig_key_offset, sig_key_length,
        iv_offset, iv_length, private_offset, private_length,
        pbp_offset, pbp_length, 0, 0, 0)  # 填充到248字节
    
    # IV 数据区域（16字节，零填充）
    iv_data = b'\x00' * 16
    
    # 签名区域（256字节，MD5 结果）
    # 计算 MD5
    import hashlib
    md5_input = head2 + padded_data + iv_data
    md5_hash = hashlib.md5(md5_input).digest()
    signature = md5_hash + b'\x00' * (256 - len(md5_hash))
    
    # 组合镜像
    image = head1 + head2 + padded_data + iv_data + signature
    
    # 写入文件
    with open(output_file, 'wb') as f:
        f.write(image)
    
    print(f"Created {output_file}: {len(image)} bytes")
    print(f"  Magic: {magic}")
    print(f"  Load address: 0x{load_address:08X}")
    print(f"  Entry point: 0x{entry_addr:08X}")
    print(f"  Bootloader size: {len(bootloader_data)} bytes")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python3 mk_aic.py <bootloader.bin> <output.aic> [load_addr] [entry_addr]")
        sys.exit(1)
    
    bootloader_bin = sys.argv[1]
    output_file = sys.argv[2]
    load_addr = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x20000000
    entry_addr = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x20000000
    
    create_aic_image(bootloader_bin, output_file, load_addr, entry_addr)
