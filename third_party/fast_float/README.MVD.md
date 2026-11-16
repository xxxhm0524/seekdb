Pinned fast_float v8.3.3

Source: https://github.com/fastfloat/fast_float/tree/dbeabeeb3e6791cb96994c2a7a084c36e62ccfb2

All nine upstream headers and all three offered license texts are retained byte-for-byte. SHA-256 values are locked in ../DEPENDENCIES.json. Only decimal-to-binary64 conversion is used; SQL/JSON grammar, finite float32 validation and range fallback remain in our parser. Nondefault floating controls use the original standard-library conversion. No storage, query engine, vector index or transaction implementation is delegated to this utility.
