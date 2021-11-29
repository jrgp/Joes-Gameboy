# Ref: https://izik1.github.io/gbops/


import json
import re
import sys
from pprint import pprint

doit = len(sys.argv) > 1 and sys.argv[1] == '-y'

ops = json.load(open('opcodes.json'))

##
## Normal opcodes
##

gen = []

for op, info in ops['unprefixed'].items():
    if info['mnemonic'] in ['DEC', 'INC']:
        if len(info['operand1']) == 1:
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      {operand1} = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] in ['BC', 'DE', 'HL']:
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      set{operand1}({operand1}() {symbol} 1);
      cycles += 4;
      break;""".format(op=op, symbol='+' if info['mnemonic'] == 'INC'  else '-', func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      {{
          byte source = cpu_read(HL());
          byte result = {func}(source);
          cpu_write(HL(), result);
      }}
      break;""".format(op=op, symbol='+' if info['mnemonic'] == 'INC'  else '-', func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == 'SP':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      {operand1}{symbol};
      cycles += 4;
      break;""".format(op=op, symbol='++' if info['mnemonic'] == 'INC' else '--', func=info['mnemonic'].lower().capitalize(), **info))
    elif info['mnemonic'] == 'LD':
        if len(info['operand1']) == 1 and len(info.get('operand2', '')) == 1:
            if info['operand1'] == info['operand2']:
                gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      break;""".format(op=op, **info))
            else:
                gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = {operand2};
      break;""".format(op=op, **info))
        elif (m := re.match('^\(([A-Z]{2})\)$', info['operand1'])) and len(info.get('operand2', '')) == 1:
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      cpu_write({reg}(), {operand2});
      break;""".format(op=op, reg=m.group(1), **info))
        elif len(info['operand1']) == 1 and (m := re.match('^\(([A-Z]{2})\)$', info.get('operand2', ''))):
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read({reg}());
      break;""".format(op=op, reg=m.group(1), **info))

        elif info['operand1'] == '(HL+)' and len(info.get('operand2', '')) == 1:
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      cpu_write(HLInc(), {operand2});
      break;""".format(op=op, **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == '(HL+)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read(HLInc());
      break;""".format(op=op, **info))

        elif info['operand1'] == '(HL-)' and len(info.get('operand2', '')) == 1:
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      cpu_write(HLDec(), {operand2});
      break;""".format(op=op, **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == '(HL-)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read(HLDec());
      break;""".format(op=op, **info))

        elif re.match('^[A-Z]{2}$', info['operand1']) and info.get('operand2') == 'd16':
            if info['operand1'] == 'SP':
                gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read16();
      break;""".format(op=op, **info))
            else:
                gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      set{operand1}(cpu_read16());
      break;""".format(op=op, **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == 'd8':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read_next();
      break;""".format(op=op, **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == '(C)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read(C + 0xFF00);
      break;""".format(op=op, **info))
        elif len(info['operand2']) == 1 and info.get('operand1') == '(C)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      cpu_write(C + 0xFF00, {operand2});
      break;""".format(op=op, **info))
        elif len(info['operand2']) == 1 and info.get('operand1') == '(a16)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      cpu_write(cpu_read16(), {operand2});
      break;""".format(op=op, **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == '(a16)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read(cpu_read16());
      break;""".format(op=op, **info))
        elif info['operand1'] == '(HL)' and info.get('operand2') == 'd8':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      cpu_write(HL(), cpu_read_next());
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'LDH':
        if len(info['operand2']) == 1 and info.get('operand1') == '(a8)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      cpu_write(cpu_read_next() + 0xFF00, {operand2});
      break;""".format(op=op, **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == '(a8)':
            gen.append("""
    // {mnemonic} {operand1} <- {operand2}
    case {op}:
      {operand1} = cpu_read(cpu_read_next() + 0xFF00);
      break;""".format(op=op, **info))

    elif info['mnemonic'] == 'ADD':
        if len(info['operand1']) == 1 and len(info.get('operand2', '')) == 1:
            gen.append("""
    // {mnemonic} {operand1} += {operand2}
    case {op}:
      {operand1} = {func}({operand2});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == 'd8':
            gen.append("""
    // {mnemonic} {operand1} += {operand2}
    case {op}:
      {operand1} = {func}(cpu_read_next());
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif len(info['operand1']) == 1 and info.get('operand2') == '(HL)':
            gen.append("""
    // {mnemonic} {operand1} += {operand2}
    case {op}:
      {operand1} = Add(cpu_read(HL()));
      break;""".format(op=op, **info))
        elif info['operand1'] == 'HL' and len(info.get('operand2')) == 2:
            # credit: https://github.com/daveallie/rustyboy/blob/master/src/register/alu.rs#L116
            gen.append("""
    // {mnemonic} {operand1} += {operand2}
    case {op}:
        {{
            word target = HL();
            word source = {source};
            word result = target + source;
            setHL(result);
            setFlag(FLAG_N, false);
            setFlag(FLAG_C, (0xFFFF-target) < source);
            setFlag(FLAG_H, (target&0x07FF)+(source&0x07FF) > 0x07FF);
        }}
      break;""".format(op=op, source=('SP' if info['operand2'] == 'SP' else info['operand2']+'()'), **info))
    elif info['mnemonic'] == 'SUB':
        if len(info['operand1']) == 1 and not info.get('operand2'):
            gen.append("""
    // {mnemonic} A -= {operand1}
    case {op}:
      A = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            gen.append("""
    // {mnemonic} A -= {operand1}
    case {op}:
      A = Sub(cpu_read(HL()));
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'AND':
        if len(info['operand1']) == 1 and not info.get('operand2'):
            gen.append("""
    // {mnemonic} A & {operand1}
    case {op}:
      A = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            gen.append("""
    // {mnemonic} A & {operand1}
    case {op}:
      A = {func}(cpu_read(HL()));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == 'd8':
            gen.append("""
    // {mnemonic} A & {operand1}
    case {op}:
      A = {func}(cpu_read_next());
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
    elif info['mnemonic'] == 'OR':
        if len(info['operand1']) == 1 and not info.get('operand2'):
            gen.append("""
    // {mnemonic} A | {operand1}
    case {op}:
      A = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            gen.append("""
    // {mnemonic} A | {operand1}
    case {op}:
      A = {func}(cpu_read(HL()));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == 'd8':
            gen.append("""
    // {mnemonic} A | {operand1}
    case {op}:
      A = {func}(cpu_read_next());
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
    elif info['mnemonic'] == 'XOR':
        if len(info['operand1']) == 1 and not info.get('operand2'):
            gen.append("""
    // {mnemonic} A ^ {operand1}
    case {op}:
      A = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            gen.append("""
    // {mnemonic} A ^ {operand1}
    case {op}:
      A = {func}(cpu_read(HL()));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == 'd8':
            gen.append("""
    // {mnemonic} A ^ {operand1}
    case {op}:
      A = {func}(cpu_read_next());
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))

    elif info['mnemonic'] == 'JR':
        if info['operand1'] == 'r8':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      offset = cpu_read_next();
      PC += (int8_t) offset;
      cycles += 4;
      break;""".format(op=op, **info))
        elif info.get('operand2') == 'r8':
            cond = {
                "NZ": "!CheckFlag(FLAG_Z)",
                "NC": "!CheckFlag(FLAG_C)",
                "Z": "CheckFlag(FLAG_Z)",
                "C": "CheckFlag(FLAG_C)",
            }[info['operand1']]
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      offset = cpu_read_next();
      if ({cond}) {{
          cycles += 4;
          PC += (int8_t) offset;
      }}
      break;""".format(op=op, cond=cond, **info))
    elif info['mnemonic'] == 'JP':
        if info['operand1'] == 'a16':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      PC = cpu_read16();
      cycles += 4;
      break;""".format(op=op, **info))
        elif info.get('operand2') == 'a16':
            cond = {
                "NZ": "!CheckFlag(FLAG_Z)",
                "NC": "!CheckFlag(FLAG_C)",
                "Z": "CheckFlag(FLAG_Z)",
                "C": "CheckFlag(FLAG_C)",
            }[info['operand1']]
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      pos = cpu_read16();
      if ({cond}) {{
          cycles += 4;
          PC = pos;
      }}
      break;""".format(op=op, cond=cond, **info))
        elif info.get('operand1') == '(HL)':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      PC = HL();
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'CALL':
        if info['operand1'] == 'a16':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      pos = cpu_read16();
      push_stack(PC);
      PC = pos;
      cycles += 12;
      break;""".format(op=op, **info))
        elif info.get('operand2') == 'a16':
            cond = {
                "NZ": "!CheckFlag(FLAG_Z)",
                "NC": "!CheckFlag(FLAG_C)",
                "Z": "CheckFlag(FLAG_Z)",
                "C": "CheckFlag(FLAG_C)",
            }[info['operand1']]
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      pos = cpu_read16();
      if ({cond}) {{
          push_stack(PC);
          PC = pos;
          cycles += 12;
      }}
      break;""".format(op=op, cond=cond, **info))
    elif info['mnemonic'] == 'RET':
        if 'operand1' in info:
            cond = {
                "NZ": "!CheckFlag(FLAG_Z)",
                "NC": "!CheckFlag(FLAG_C)",
                "Z": "CheckFlag(FLAG_Z)",
                "C": "CheckFlag(FLAG_C)",
            }[info['operand1']]
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      cycles += 4;
      if ({cond}) {{
          cycles += 12;
          PC = pop_stack();
      }}
      break;""".format(op=op, cond=cond, **info))
        else:
            gen.append("""
    // {mnemonic}
    case {op}:
      cycles += 12;
      PC = pop_stack();
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'RETI':
        gen.append("""
    // {mnemonic}
    case {op}:
      cycles += 12;
      PC = pop_stack();
      interrupts = true;
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'PUSH':
        gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      push_stack({operand1}());
      cycles += 12;
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'POP':
        gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      set{operand1}(pop_stack());
      cycles += 8;
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'RLA':
            gen.append("""
    // {mnemonic}
    case {op}:
      A = RL(A);
      setFlag(FLAG_Z, false);
      break;""".format(op=op, func=info['mnemonic'], **info))
    elif info['mnemonic'] == 'CP':
        if len(info['operand1']) == 1:
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'], **info))
        elif info['operand1'] == 'd8':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      {func}(cpu_read_next());
      break;""".format(op=op, func=info['mnemonic'], **info))
        elif info['operand1'] == '(HL)':
            gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      {func}(cpu_read(HL()));
      break;""".format(op=op, func=info['mnemonic'], **info))
    elif info['mnemonic'] == 'DI':
        gen.append("""
    // {mnemonic}
    case {op}:
      interrupts = false;
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'EI':
        gen.append("""
    // {mnemonic}
    case {op}:
      interrupts = true;
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'CPL':
        gen.append("""
    // {mnemonic}
    case {op}:
      A = ~A;
      break;""".format(op=op, **info))
    elif info['mnemonic'] == 'RST':
        gen.append("""
    // {mnemonic} {operand1}
    case {op}:
      push_stack(PC);
      cycles += 12;
      PC = 0x{loc};
      break;""".format(op=op, loc=info['operand1'][:2], func=info['mnemonic'], **info))

if doit:
    path = 'gb.c'
    new = []
    hit = False
    for line in open(path):
        line = line.rstrip()
        if 'START GENERATED' in line:
            hit = True
            new.append('// START GENERATED')
            new.extend(gen)
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
    print('\n'.join(gen))
    print('// Generated %d ops' % len(gen))

##
## Extended opcodes
##

genex = []

for op, info in ops['cbprefixed'].items():
    if info['mnemonic'] == 'BIT':
        if len(info['operand1']) == 1 and len(info['operand2']) == 1:
            genex.append("""
    // {mnemonic} {operand1} of {operand2}
    case {op}:
      {func}({operand2}, {operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand2'] == '(HL)':
            genex.append("""
    // {mnemonic} {operand1} of {operand2}
    case {op}:
      {func}(cpu_read(HL()), {operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
    elif info['mnemonic'] == 'RES':
        if len(info['operand1']) == 1 and len(info['operand2']) == 1:
            genex.append("""
    // {mnemonic} {operand1} of {operand2}
    case {op}:
      {operand2} = bit_clear({operand2}, {operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand2'] == '(HL)':
            genex.append("""
    // {mnemonic} {operand1} of {operand2}
    case {op}:
      cpu_write(HL(), bit_clear(cpu_read(HL()), {operand1}));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))

    elif info['mnemonic'] == 'SET':
        if len(info['operand1']) == 1 and len(info['operand2']) == 1:
            genex.append("""
    // {mnemonic} {operand1} of {operand2}
    case {op}:
      {operand2} = bit_set({operand2}, {operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand2'] == '(HL)':
            genex.append("""
    // {mnemonic} {operand1} of {operand2}
    case {op}:
      cpu_write(HL(), bit_set(cpu_read(HL()), {operand1}));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
    elif info['mnemonic'] == 'RL':
        if len(info['operand1']) == 1:
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      {operand1} = RL({operand1});
      break;""".format(op=op, func=info['mnemonic'], **info))
        elif info['operand1'] == '(HL)':
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      cpu_write(HL(), RL(cpu_read(HL())));
      break;""".format(op=op, func=info['mnemonic'], **info))
    elif info['mnemonic'] == 'SWAP':
        if len(info['operand1']) == 1:
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      {operand1} = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      cpu_write(HL(), {func}(cpu_read(HL())));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
    elif info['mnemonic'] == 'SLA':
        if len(info['operand1']) == 1:
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      {operand1} = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      cpu_write(HL(), {func}(cpu_read(HL())));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
    elif info['mnemonic'] == 'SRL':
        if len(info['operand1']) == 1:
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      {operand1} = {func}({operand1});
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))
        elif info['operand1'] == '(HL)':
            genex.append("""
    // {mnemonic} {operand1}
    case {op}:
      cpu_write(HL(), {func}(cpu_read(HL())));
      break;""".format(op=op, func=info['mnemonic'].lower().capitalize(), **info))



if doit:
    path = 'gb.c'
    new = []
    hit = False
    for line in open(path):
        line = line.rstrip()
        if 'START EX GENERATED' in line:
            hit = True
            new.append('// START EX GENERATED')
            new.extend(genex)
            new.append('')
            new.append('// END EX GENERATED')
        else:
            if hit:
                if 'END EX GENERATED' in line:
                    hit = False
            else:
                new.append(line)
    open(path, 'w').write('\n'.join(new)+'\n')
else:
    print('\n'.join(genex))
    print('// Generated %d ex ops' % len(genex))
