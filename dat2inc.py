import re

print("""static struct {
    const char* name;
    uint16_t size;
    uint32_t crc;
} __packed database[] = {""")

longest = 0

with open("2600.dat") as f:
    for line in f:
        m = re.match(r'\s*rom \( name "([^"]+)\.[^"]*" size ([0-9]+) crc ([0-9A-Fa-f]+) .*', line)
        if m:
            size = int(m.group(2))
            if size <= 32768:
                fname = re.sub(r' \(.*', '', m.group(1))
                longest = max(len(fname), longest)
                print('{ "%s", %s, 0x%s },' % (m.group(1),m.group(2),m.group(3)))

print("};")
print("#define LONGEST_FILENAME %u" % (longest+4))
