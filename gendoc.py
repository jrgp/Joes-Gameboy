import json
import sys

doit = len(sys.argv) > 1 and sys.argv[1] == '-y'

ops = json.load(open('opcodes.json'))

gen = {}

for op, info in ops['unprefixed'].items():
    args = []
    if info.get('operand1'):
        args.append(info['operand1'])
    if info.get('operand2'):
        args.append(info['operand2'])
    doc = info['mnemonic']
    if args:
        doc += ' ' + ', '.join(args)
    gen[int(op, 16)] = doc


generated = ["""char* opnames[256] = {"""]

for x in range(256):
    generated.append('"'+gen.get(x, '')+'",')

generated.append("""};""")

genex = {}

for op, info in ops['cbprefixed'].items():
    args = []
    if info.get('operand1'):
        args.append(info['operand1'])
    if info.get('operand2'):
        args.append(info['operand2'])
    doc = info['mnemonic']
    if args:
        doc += ' ' + ', '.join(args)
    genex[int(op, 16)] = doc

generated.append("""char* exopnames[256] = {""")

for x in range(256):
    generated.append('"'+genex.get(x, '')+'",')

generated.append("""};""")

if doit:
    path = 'opnames.h'
    new = []
    hit = False
    for line in open(path):
        line = line.rstrip()
        if 'START GENERATED' in line:
            hit = True
            new.append('// START GENERATED')
            new.extend(generated)
            new.append('')
            new.append('// END GENERATED')
        else:
            if hit:
                if 'END GENERATED' in line:
                    hit = False
            else:
                new.append(line)
    open(path, 'w').write('\n'.join(new)+'\n')
else:
    print('\n'.join(generated))

