# awk

CriSH's `awk` is a POSIX awk. It is the language `awk` and `mawk` implement,
not the superset `gawk` does — see the bottom of this page for what that costs
you.

## The program

```
pattern { action }
pattern
{ action }
function name(params) { body }
```

Patterns:

| | |
| --- | --- |
| `BEGIN` | before any input is read |
| `END` | after all input is read |
| *expression* | run the action when it is true |
| `/regex/` | run the action when `$0` matches |
| *pat1*`,`*pat2* | a range, from the first match to the next match of pat2 |

A pattern with no action means `{ print }`. An action with no pattern runs for
every record.

## Records and fields

`$0` is the record, `$1`…`$NF` the fields. Assigning to a field rebuilds `$0`
using `OFS`; assigning to `$0` re-splits the fields; assigning to `NF` truncates
or extends the record.

Splitting follows `FS`:

| `FS` | meaning |
| --- | --- |
| `" "` (the default) | runs of blanks and newlines, leading and trailing ignored |
| a single character | that literal character |
| longer than one character | an extended regular expression |

`RS` accepts a single character, the empty string (paragraph mode) or a regular
expression.

## Variables

`NR NF FNR FS OFS ORS RS FILENAME SUBSEP RSTART RLENGTH CONVFMT OFMT ARGC ARGV
ENVIRON`

## Expressions

Assignment `= += -= *= /= %= ^=` · ternary `?:` · `||` `&&` · `in` ·
`~` `!~` · `< <= > >= != ==` · concatenation by juxtaposition ·
`+ -` · `* / %` · unary `! - +` · `^` (right associative) ·
`++ --` prefix and postfix · `$` · grouping.

Comparison is numeric when both sides are numbers or look like numbers as they
came out of the input; otherwise it is a string comparison. That is the POSIX
rule, and it is why `$1 == "10"` and `$1 == 10` can differ.

## Statements

`if`/`else`, `while`, `do`/`while`, `for(init; cond; step)`, `for (k in a)`,
`break`, `continue`, `next`, `nextfile`, `exit [n]`, `return [expr]`,
`delete a[k]`, `delete a`, `{ }`, `;`.

## Output

```awk
print                     # $0
print a, b                # separated by OFS, terminated by ORS
printf "%5.2f\n", x
print x > "file"
print x >> "file"
print x | "sort -n"
```

`printf` supports `%d %i %o %u %x %X %e %E %f %F %g %G %a %A %c %s %%`, flags
`- + space # 0`, a width, a precision, and `*` for either.

## getline

| | |
| --- | --- |
| `getline` | next record into `$0`, updating `NR` and `NF` |
| `getline var` | next record into `var`, updating `NR` |
| `getline < "file"` | next record of file into `$0` |
| `getline var < "file"` | next record of file into `var` |
| `"cmd" \| getline` | next line of a command into `$0` |
| `"cmd" \| getline var` | next line of a command into `var` |

Each returns 1 on a record, 0 at end of input, -1 on an error.

## Built-in functions

**Strings:** `length([x])` `substr(s, m [, n])` `index(s, t)`
`split(s, a [, fs])` `sub(re, rep [, target])` `gsub(re, rep [, target])`
`match(s, re)` `sprintf(fmt, ...)` `tolower(s)` `toupper(s)`

**Numbers:** `sin cos atan2 exp log sqrt int rand srand`

**Other:** `system(cmd)` `close(name)` `fflush()`

In `sub` and `gsub`, `&` in the replacement is the matched text and `\&` is a
literal ampersand. `match` sets `RSTART` and `RLENGTH`.

## Arrays

Associative, always. `a[i, j]` joins the subscripts with `SUBSEP`, and
`(i, j) in a` tests for that key. `for (k in a)` visits keys in an unspecified
order — sort them if the order matters:

```awk
{ count[$1]++ }
END { for (k in count) printf "%s\t%d\n", k, count[k] }
```

```sh
crish -c 'awk "{c[\$1]++} END{for(k in c) print c[k], k}" access.log | sort -rn | head'
```

Arrays are passed to functions by reference, scalars by value, exactly as POSIX
says.

## What is missing compared with gawk

`asort` `asorti` `gensub` `strftime` `systime` `mktime` `patsplit` `toupper`
on multibyte input, `BEGINFILE`/`ENDFILE`, `ENVIRON` writes, `--profile`,
`-M`, `RT`, `IGNORECASE`, `FIELDWIDTHS`, `FPAT`, two-way pipes with `|&`.

If a script uses those, it needs gawk; CriSH will report a syntax error at the
line rather than doing something almost right.

## Command line

```
awk [-F fs] [-v var=value] 'program' [file | var=value ...]
awk [-F fs] [-v var=value] -f progfile [file | var=value ...]
```

`-F t` means a tab, as in gawk. A `var=value` operand between file names is
applied when awk reaches that point in the argument list.
