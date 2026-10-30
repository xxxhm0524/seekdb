-- Differential regression inputs: capture complete results on the parent and
-- candidate using the same client, charset and session settings, then compare.
-- This script is provided for later execution; it has not been run.
SET NAMES utf8mb4;
SELECT TOKENIZE('', 'space');
SELECT TOKENIZE('', 'beng');
SELECT TOKENIZE('', 'ik');
SELECT TOKENIZE('!!! ??? --- ...', 'space');
SELECT TOKENIZE('!!! ??? --- ...', 'beng');
SELECT TOKENIZE('!!! ??? --- ...', 'ik');
SELECT TOKENIZE('Alpha ALPHA alpha alpha123 0123456789', 'beng');
SELECT TOKENIZE('Alpha ALPHA alpha alpha123 0123456789', 'ik');
SELECT TOKENIZE('alpha,beta;gamma.delta!epsilon?zeta', 'space');
SELECT TOKENIZE(CONCAT('alpha', CHAR(9), 'beta', CHAR(13), CHAR(10), 'gamma'), 'beng');
SELECT TOKENIZE(CONCAT('alpha', CHAR(9), 'beta', CHAR(13), CHAR(10), 'gamma'), 'space');
SELECT TOKENIZE('数据库全文检索 OceanBase seekdb 2026', 'ik');
SELECT TOKENIZE('ＡＢＣａｂｃ１２３ 中文，标点。かな カナ 한글 😀', 'ik');
SELECT TOKENIZE('café naïve résumé 中文 😀 alpha', 'beng');
SELECT TOKENIZE(REPEAT('alpha', 100), 'beng');
SELECT TOKENIZE(CONCAT(REPEAT('!', 100), 'alpha', REPEAT('?', 100)), 'beng');
SELECT TOKENIZE(REPEAT('!', 512), 'beng');
SELECT TOKENIZE(NULL, 'beng');
SELECT TOKENIZE(NULL, 'ik');
-- ASCII fast path boundaries and transitions into multibyte input.
SELECT TOKENIZE('a z A Z 0 9 @ [ ` { / :', 'ik');
SELECT TOKENIZE('lowercase words repeated repeated 123 abc123', 'beng');
SELECT TOKENIZE('lower UPPER Mixed lower中文lower 😀lower', 'beng');
SELECT TOKENIZE('lower UPPER Mixed lower中文lower 😀lower', 'ik');
SELECT TOKENIZE(CONCAT('a', CHAR(0), 'b', CHAR(127), 'c'), 'space');
SELECT TOKENIZE(CONCAT('a', CHAR(0), 'b', CHAR(127), 'c'), 'beng');
