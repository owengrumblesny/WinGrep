# cython: language_level=3, boundscheck=False, wraparound=False
"""A small regex engine: patterns compile to a Thompson NFA.

Every possible match is followed at the same time, one input byte at a
step, so patterns like (a+)+b cannot blow up the way backtracking can.
Supports literals, ., *, +, ?, |, () and backslash escapes, on bytes.
"""

from libc.stdlib cimport malloc, free
from libc.string cimport memset

cdef enum:
    CHAR    # Match the byte x, then go to the next instruction.
    ANY     # Match any byte but newline, then go to the next instruction.
    SPLIT   # Continue at both x and y.
    JMP     # Continue at x.
    MATCH   # Success.

cdef struct Inst:
    int op
    int x
    int y


class _Parser:
    """Turn a pattern into a list of (op, x, y) instructions.

    Jump targets are relative, so fragments can simply be concatenated.
    """

    def __init__(self, pattern):
        self.p = pattern
        self.pos = 0

    def peek(self):
        return self.p[self.pos] if self.pos < len(self.p) else None

    def alternation(self):
        left = self.sequence()
        while self.peek() == ord('|'):
            self.pos += 1
            right = self.sequence()
            left = ([(SPLIT, 1, len(left) + 2)] + left
                    + [(JMP, len(right) + 1, 0)] + right)
        return left

    def sequence(self):
        code = []
        while self.peek() is not None and self.peek() not in b'|)':
            code += self.repeat()
        return code

    def repeat(self):
        code = self.atom()
        while self.peek() is not None and self.peek() in b'*+?':
            op = self.p[self.pos]
            self.pos += 1
            n = len(code)
            if op == ord('*'):
                code = [(SPLIT, 1, n + 2)] + code + [(JMP, -(n + 1), 0)]
            elif op == ord('+'):
                code = code + [(SPLIT, -n, 1)]
            else:
                code = [(SPLIT, 1, n + 1)] + code
        return code

    def atom(self):
        c = self.p[self.pos]
        self.pos += 1
        if c == ord('('):
            code = self.alternation()
            if self.peek() != ord(')'):
                raise ValueError('missing )')
            self.pos += 1
            return code
        if c == ord('.'):
            return [(ANY, 0, 0)]
        if c in b'*+?':
            raise ValueError('nothing to repeat')
        if c == ord('\\'):
            if self.peek() is None:
                raise ValueError('trailing backslash')
            c = self.p[self.pos]
            self.pos += 1
        return [(CHAR, c, 0)]


cdef void add_thread(const Inst *prog, int *threads, int *count, int *mark,
                     int gen, int *stack, int pc) noexcept nogil:
    """Add PC to THREADS, following jumps, skipping those seen this step."""
    cdef int top = 1
    stack[0] = pc
    while top:
        top -= 1
        pc = stack[top]
        if mark[pc] == gen:
            continue
        mark[pc] = gen
        if prog[pc].op == JMP:
            stack[top] = pc + prog[pc].x
            top += 1
        elif prog[pc].op == SPLIT:
            stack[top] = pc + prog[pc].y
            stack[top + 1] = pc + prog[pc].x
            top += 2
        else:
            threads[count[0]] = pc
            count[0] += 1


cdef bint run(const Inst *prog, int n, const unsigned char *s,
              Py_ssize_t size, int *memory) noexcept nogil:
    """Return true if PROG matches anywhere in S."""
    cdef int *cur = memory
    cdef int *nxt = memory + n
    cdef int *mark = memory + 2 * n
    cdef int *stack = memory + 3 * n
    cdef int *swap
    cdef int ncur = 0, nnxt, gen = 1, t, pc
    cdef Py_ssize_t i

    memset(mark, 0, n * sizeof(int))
    for i in range(size + 1):
        # Starting a new thread at every position finds matches anywhere.
        add_thread(prog, cur, &ncur, mark, gen, stack, 0)
        gen += 1
        nnxt = 0
        for t in range(ncur):
            pc = cur[t]
            if prog[pc].op == MATCH:
                return True
            if i < size and (
                    (prog[pc].op == CHAR and s[i] == prog[pc].x)
                    or (prog[pc].op == ANY and s[i] != 10)):
                add_thread(prog, nxt, &nnxt, mark, gen, stack, pc + 1)
        swap = cur
        cur = nxt
        nxt = swap
        ncur = nnxt
    return False


cdef class Pattern:
    """A compiled regex: Pattern(b'colou?r').search(b'my color') -> True"""

    cdef Inst *prog
    cdef int *memory
    cdef int n

    def __cinit__(self, pattern):
        if isinstance(pattern, str):
            pattern = pattern.encode('utf-8')
        parser = _Parser(pattern)
        code = parser.alternation()
        if parser.peek() is not None:
            raise ValueError('unmatched )')
        code.append((MATCH, 0, 0))

        self.n = len(code)
        self.prog = <Inst *> malloc(self.n * sizeof(Inst))
        self.memory = <int *> malloc((5 * self.n + 2) * sizeof(int))
        if not self.prog or not self.memory:
            raise MemoryError()
        for i, (op, x, y) in enumerate(code):
            self.prog[i].op = op
            self.prog[i].x = x
            self.prog[i].y = y

    def __dealloc__(self):
        free(self.prog)
        free(self.memory)

    def search(self, data):
        """Return True if the pattern matches anywhere in DATA."""
        cdef const unsigned char[:] view
        if isinstance(data, str):
            data = data.encode('utf-8')
        view = data
        if view.shape[0] == 0:
            return run(self.prog, self.n, <const unsigned char *> b'', 0,
                       self.memory)
        with nogil:
            result = run(self.prog, self.n, &view[0], view.shape[0],
                         self.memory)
        return result
