import string

byte_to_c_string_table = {c: chr(c) for c in range(32, 127)}
byte_to_c_string_table[0x22] = '\\"'
byte_to_c_string_table[0x5c] = '\\\\'
byte_to_c_string_table[0x07] = '\\a'
byte_to_c_string_table[0x08] = '\\b'
byte_to_c_string_table[0x0c] = '\\f'
byte_to_c_string_table[0x0a] = '\\n'
byte_to_c_string_table[0x0d] = '\\r'
byte_to_c_string_table[0x09] = '\\t'
byte_to_c_string_table[0x0b] = '\\v'

digit_bytes = set(ord(c) for c in string.digits)


def byte_to_c_string(b, next_b=None):
    if b in byte_to_c_string_table:
        return byte_to_c_string_table[b]
    elif next_b in digit_bytes:
        return '\\' + format(b, '03o')
    else:
        return '\\' + format(b, 'o')
    

def pairwise(iterable):
    it = iter(iterable)
    a = next(it)
    for b in it:
        yield a, b
        a = b


def bytes_to_c_string(bytes):
    # x is a dummy byte
    return '"' + ''.join(byte_to_c_string(b, next_b) for b, next_b in pairwise(bytes + b'x')) + '"'
