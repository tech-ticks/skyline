# Create a binary symbol map file from an IDA/Ghidra exported .map file

import struct
import sys

FILTERED_SYMS = ['CustomAttributesCacheGenerator', 'RuntimeInvoker_', 'XmlSchema', 'Array_InternalArray_', 'jpt_', 'def_', 'sub_', 'Array_Resize_', 'Array_Reverse_', 'Array_Sort_']

if len(sys.argv) < 2:
	print("Syntax: python3 convertSymbolMap.py <map> <out>")
	sys.exit()

map_file = sys.argv[1];
out_file = sys.argv[2];

syms = []

with open(map_file, "r") as f:
	while (line := f.readline()):
		line = line.strip()
		if line.startswith("Address"):
			break
		section_data = list(filter(None, line.split(' ')))
		if len(section_data) < 3:
			continue
		id_and_start = section_data[0]
		if not ':' in id_and_start:
			continue
		id = int(id_and_start.split(':')[0], base=16)
		start = int(id_and_start.split(':')[1], base=16)
		assert start == 0, 'Only a "start" value of zero is currently supported.'
		length = int(section_data[1].rstrip('H'), base=16)
		name = section_data[2]
		print(f"id={id}, start={start}, length={length}, name={name}")

	while (line := f.readline()):
		line = line.strip()
		section_data = list(filter(None, line.split(' ')))
		if len(section_data) < 2:
			continue

		section_id_and_offset = section_data[0]
		section_id = int(section_id_and_offset.split(':')[0], base=16)
		offset = int(section_id_and_offset.split(':')[1], base=16)
		sym_name = section_data[1]

		included = True
		for filt in FILTERED_SYMS:
			if filt in sym_name:
				included = False

		if included:
			syms.append((offset, sym_name))

with open(out_file, "wb") as f:
	f.write(struct.pack("i", len(syms)))
	for sym in syms:
		(offset, sym_name) = sym
		f.write(struct.pack('i', offset))
		f.write(sym_name.encode('ascii'))
		f.write(b'\0')
