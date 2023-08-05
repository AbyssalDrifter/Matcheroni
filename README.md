------
### Update 7/18/2023 - Matcheroni is getting a major rewrite plus a parsing framework I'm calling Parseroni. There will be a full live interactive tutorial as well. Please hold off on using this library until those are complete.

------

# Matcheroni

[Matcheroni](https://github.com/aappleby/Matcheroni/blob/main/matcheroni/Matcheroni.hpp) is a minimalist, zero-dependency, single-header C++20 library for doing pattern matching using [Parsing Expression Grammars](https://en.wikipedia.org/wiki/Parsing_expression_grammar) (PEGs). PEGs are similar to regular expressions, but both more and less powerful.

[Parseroni](https://github.com/aappleby/Matcheroni/blob/main/matcheroni/Parseroni.hpp) is a companion single-header library that can capture the content of Matcheroni patterns and assemble them into concrete [parse trees](https://en.wikipedia.org/wiki/Parse_tree).

Together, Matcheroni and Parseroni generate tiny and fast parsers that are easy to customize and integrate into your existing codebase.

A tutorial for building a JSON parser in Matcheroni+Parseroni can be found [here](https://aappleby.github.io/Matcheroni/tutorial)

Documentation for Matcheroni can be found [here](https://aappleby.github.io/Matcheroni)

# Building the Matcheroni examples and tests
Install [Ninja](https://ninja-build.org/) if you haven't already, then run ninja in the repo root. The test suite will run as part of the build process.

To rebuild the emscriptened tutorial, run "build -f build_docs.ninja" in the repo root.

See build.ninja for configuration options.

# Performance

The JSON parser example contains a benchmark that parses the test files from [nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark) 100 times and reports the median parse time.

When built with "-O3 -flto", the benchmark can parse the three test files in about 4.5 milliseconds on my Ryzen 5900x (or around a gigabyte per second) - quite competitive, though not quite apples-to-apples as Parseroni does not automatically convert numeric fields from text to doubles. Adding a quick-and-dirty atof() implementation slows things down to ~900 megabytes per second.

The parser itself is about 10k of machine code and compiles in a few hundred milliseconds.

Comparing it with RapidJSON's built-in benchmark also produces favorable numbers (but again, be aware of apples-to-oranges differences):

| Benchmark                                         | Result      |
| ------------------------------------------------- | ----------- |
| Matcheroni+Parseroni json_benchmark -O3 -flto     | 1.78 gb/sec |
| RapidJSON DocumentParse_MemoryPoolAllocator       | 1.30 gb/sec |
| RapidJSON DocumentParse_MemoryPoolAllocator_SSE42 | 2.85 gb/sec |

# Caveats

Matcheroni requires C++20, which is a non-starter for some projects. There's not a lot I can do about that, as I'm heavily leveraging some newish template stuff that doesn't have any backwards-compatible equivalents.

Like parsing expression grammars, matchers are greedy - ```Seq<Some<Atom<'a'>>, Atom<'a'>>``` will _always_ fail as ```Some<Atom<'a'>>``` leaves no 'a's behind for the second ```Atom<'a'>``` to match.

Matcheroni does not implement any form of [packrat parsing](https://pdos.csail.mit.edu/~baford/packrat/icfp02/), though it could be added on top. Trying to do [operator-precedence parsing](https://en.wikipedia.org/wiki/Operator-precedence_parser) using the precedence-climbing method will be unbearably slow due to the huge number of recursive calls that don't end up matching anything.

Recursive matchers create recursive code that can explode your call stack.

Left-recursive matchers can get stuck in an infinite loop - this is true with most versions of Parsing Expression Grammars, it's a fundamental limitation of the algorithm.

# A Particularly Large Matcheroni Pattern

Here's the code I use to match C99 integers, plus a few additions from the C++ spec and the GCC extensions.

Note that it consists of 20 ```using``` declarations and the only actual "code" is ```return integer_constant::match(ctx, body);```

If you follow along in Appendix A of the [C99 spec](https://www.open-std.org/jtc1/sc22/wg14/www/docs/n1256.pdf), you'll see it lines up quite closely.

```cpp
TextSpan match_int(TextMatchContext& ctx, TextSpan body) {
  // clang-format off
  using digit                = Range<'0', '9'>;
  using nonzero_digit        = Range<'1', '9'>;

  using decimal_constant     = Seq<nonzero_digit, Any<ticked<digit>>>;

  using hexadecimal_prefix         = Oneof<Lit<"0x">, Lit<"0X">>;
  using hexadecimal_digit          = Range<'0','9','a','f','A','F'>;
  using hexadecimal_digit_sequence = Seq<hexadecimal_digit, Any<ticked<hexadecimal_digit>>>;
  using hexadecimal_constant       = Seq<hexadecimal_prefix, hexadecimal_digit_sequence>;

  using binary_prefix         = Oneof<Lit<"0b">, Lit<"0B">>;
  using binary_digit          = Atom<'0','1'>;
  using binary_digit_sequence = Seq<binary_digit, Any<ticked<binary_digit>>>;
  using binary_constant       = Seq<binary_prefix, binary_digit_sequence>;

  using octal_digit        = Range<'0', '7'>;
  using octal_constant     = Seq<Atom<'0'>, Any<ticked<octal_digit>>>;

  using unsigned_suffix        = Atom<'u', 'U'>;
  using long_suffix            = Atom<'l', 'L'>;
  using long_long_suffix       = Oneof<Lit<"ll">, Lit<"LL">>;
  using bit_precise_int_suffix = Oneof<Lit<"wb">, Lit<"WB">>;

  // This is begin little odd because we have to match in longest-suffix-first order
  // to ensure we capture the entire suffix
  using integer_suffix = Oneof<
    Seq<unsigned_suffix,  long_long_suffix>,
    Seq<unsigned_suffix,  long_suffix>,
    Seq<unsigned_suffix,  bit_precise_int_suffix>,
    Seq<unsigned_suffix>,

    Seq<long_long_suffix,       Opt<unsigned_suffix>>,
    Seq<long_suffix,            Opt<unsigned_suffix>>,
    Seq<bit_precise_int_suffix, Opt<unsigned_suffix>>
  >;

  // GCC allows i or j in addition to the normal suffixes for complex-ified types :/...
  using complex_suffix = Atom<'i', 'j'>;

  // Octal has to be _after_ bin/hex so we don't prematurely match the prefix
  using integer_constant =
  Seq<
    Oneof<
      decimal_constant,
      hexadecimal_constant,
      binary_constant,
      octal_constant
    >,
    Seq<
      Opt<complex_suffix>,
      Opt<integer_suffix>,
      Opt<complex_suffix>
    >
  >;

  return integer_constant::match(ctx, body);
  // clang-format on
}
```
