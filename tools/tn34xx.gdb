# GDB helper for the tn34xx GDB stub.
#
#   gdb-multiarch -x tools/tn34xx.gdb -ex 'target remote :1234'
#   gdb-multiarch -x tools/tn34xx.gdb        # then: tnc [port]
#
# Can be sourced before connecting; nothing here talks to the target until a
# command is used or the target stops.
#
# Address model (see src/gdb/gdb_stub.hpp): GDB knows nothing about real-mode
# segmentation, so the stub presents a flat view.
#   - $eip / $pc is the *linear* address of CS:IP (CS * 16 + IP), not IP.
#     Writing $pc moves IP within the current CS only.
#   - Every memory address (x, break *ADDR, watch, ...) is linear.
#   - $cs $ds $es $ss hold the real segment values.
# The seg:off commands below do the CS*16+IP arithmetic for you:
#
#   lin  SEG OFF          print the linear address of SEG:OFF
#   bseg SEG OFF          breakpoint at SEG:OFF
#   useg SEG OFF [N]      disassemble N (default 10) instructions at SEG:OFF
#   wseg SEG OFF [LEN]    write watchpoint on LEN (default 1) bytes at SEG:OFF
#   tnc  [PORT]           target remote :PORT (default 1234)
#
# SEG and OFF are hexadecimal like in the tn34xx monitor ("f000", "0x54",
# "54h"; "#123" is decimal) or a register name ("cs", "ds", "si", ...).
# "SEG:OFF" as a single argument also works. Anything else is evaluated as a
# GDB expression. Without Python, simple CLI versions are defined instead;
# they take GDB expressions (write 0xf000, not f000).
#
# On every stop the hook prints the tn34xx register view ("monitor regs",
# which includes the current instruction) and the next few instructions.
# "monitor <cmd>" runs any tn34xx monitor command (see docs/monitor.md).

set osabi none
set architecture i8086
set disassembly-flavor intel
set pagination off
set print pretty on
set print array on
set print elements 256
set print symbol off

define hook-stop
  monitor regs
  x/4i $pc
end

# --- CLI fallbacks (replaced by the Python versions below when available) ---

define lin
  if $argc != 2
    echo usage: lin SEG OFF\n
  else
    printf "%04x:%04x = 0x%05x\n", ($arg0) & 0xffff, ($arg1) & 0xffff, ((($arg0) & 0xffff) << 4) + (($arg1) & 0xffff)
  end
end
document lin
Print the linear address of SEG:OFF.  Usage: lin SEG OFF
end

define bseg
  if $argc != 2
    echo usage: bseg SEG OFF\n
  else
    break *(((($arg0) & 0xffff) << 4) + (($arg1) & 0xffff))
  end
end
document bseg
Set a breakpoint at SEG:OFF.  Usage: bseg SEG OFF
end

define useg
  if $argc == 2
    x/10i ((($arg0) & 0xffff) << 4) + (($arg1) & 0xffff)
  else
    if $argc == 3
      eval "x/%di 0x%x", $arg2, ((($arg0) & 0xffff) << 4) + (($arg1) & 0xffff)
    else
      echo usage: useg SEG OFF [N]\n
    end
  end
end
document useg
Disassemble N (default 10) instructions at SEG:OFF.  Usage: useg SEG OFF [N]
end

define wseg
  if $argc == 2
    watch *(unsigned char *)(((($arg0) & 0xffff) << 4) + (($arg1) & 0xffff))
  else
    if $argc == 3
      eval "watch *(unsigned char (*)[%d])0x%x", $arg2, ((($arg0) & 0xffff) << 4) + (($arg1) & 0xffff)
    else
      echo usage: wseg SEG OFF [LEN]\n
    end
  end
end
document wseg
Set a write watchpoint on LEN (default 1) bytes at SEG:OFF.  Usage: wseg SEG OFF [LEN]
end

define tnc
  if $argc == 0
    target remote :1234
  else
    eval "target remote :%d", $arg0
  end
end
document tnc
Connect to the tn34xx GDB stub.  Usage: tnc [PORT]  (default 1234)
end

# --- Python versions with monitor-style argument parsing ---
# Kept last: in a GDB without Python this block fails and stops sourcing,
# leaving the CLI versions above in place.

python
import gdb

_REGS = {"ax", "bx", "cx", "dx", "si", "di", "sp", "bp", "cs", "ds", "es", "ss",
         "ip", "pc", "eip", "flags", "eflags"}


def _tn_value(tok):
    """Parse a monitor-style number, register name or GDB expression."""
    t = tok.strip()
    low = t.lower()
    if low == "ip":
        return (_tn_value("pc") - (_tn_value("cs") << 4)) & 0xffff
    if low in _REGS:
        if low in ("ax", "bx", "cx", "dx", "si", "di", "sp", "bp"):
            low = "e" + low
        elif low == "flags":
            low = "eflags"
        return int(gdb.parse_and_eval("$" + low)) & 0xffffffff
    if low.startswith("#"):
        return int(low[1:], 10)
    s = low[2:] if low.startswith("0x") else low
    if s.endswith("h"):
        s = s[:-1]
    try:
        return int(s, 16)
    except ValueError:
        return int(gdb.parse_and_eval(t))


def _tn_segoff(argv, cmd, extra):
    """Split argv into (seg, off, rest); accepts 'SEG OFF' or 'SEG:OFF'."""
    if argv and ":" in argv[0]:
        seg, off = argv[0].split(":", 1)
        argv = [seg, off] + argv[1:]
    if len(argv) < 2 or len(argv) > 2 + extra:
        raise gdb.GdbError("usage: " + cmd)
    seg = _tn_value(argv[0]) & 0xffff
    off = _tn_value(argv[1]) & 0xffff
    return seg, off, argv[2:]


class _TnCommand(gdb.Command):
    usage = ""
    extra = 0

    def __init__(self, name):
        super().__init__(name, gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        seg, off, rest = _tn_segoff(gdb.string_to_argv(arg), self.usage, self.extra)
        self.run(seg, off, (seg << 4) + off, rest)


class TnLin(_TnCommand):
    """Print the linear address of SEG:OFF.  Usage: lin SEG OFF"""
    usage = "lin SEG OFF"

    def __init__(self):
        super().__init__("lin")

    def run(self, seg, off, lin, rest):
        print("%04x:%04x = 0x%05x" % (seg, off, lin))


class TnBseg(_TnCommand):
    """Set a breakpoint at SEG:OFF.  Usage: bseg SEG OFF"""
    usage = "bseg SEG OFF"

    def __init__(self):
        super().__init__("bseg")

    def run(self, seg, off, lin, rest):
        gdb.execute("break *0x%x" % lin)


class TnUseg(_TnCommand):
    """Disassemble N (default 10) instructions at SEG:OFF.  Usage: useg SEG OFF [N]"""
    usage = "useg SEG OFF [N]"
    extra = 1

    def __init__(self):
        super().__init__("useg")

    def run(self, seg, off, lin, rest):
        n = int(_tn_value(rest[0])) if rest else 10
        gdb.execute("x/%di 0x%x" % (n, lin))


class TnWseg(_TnCommand):
    """Set a write watchpoint on LEN (default 1) bytes at SEG:OFF.  Usage: wseg SEG OFF [LEN]"""
    usage = "wseg SEG OFF [LEN]"
    extra = 1

    def __init__(self):
        super().__init__("wseg")

    def run(self, seg, off, lin, rest):
        n = int(_tn_value(rest[0])) if rest else 1
        if n <= 0:
            raise gdb.GdbError("LEN must be positive")
        types = {1: "unsigned char", 2: "unsigned short", 4: "unsigned int"}
        if n in types:
            gdb.execute("watch *(%s *)0x%x" % (types[n], lin))
        else:
            gdb.execute("watch *(unsigned char (*)[%d])0x%x" % (n, lin))


class TnConnect(gdb.Command):
    """Connect to the tn34xx GDB stub.  Usage: tnc [PORT|HOST:PORT]  (default :1234)"""

    def __init__(self):
        super().__init__("tnc", gdb.COMMAND_USER)

    def invoke(self, arg, from_tty):
        a = arg.strip() or "1234"
        gdb.execute("target remote %s" % (a if ":" in a else ":" + a))


TnLin()
TnBseg()
TnUseg()
TnWseg()
TnConnect()
end
