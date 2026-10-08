// Builds main/setup_page.html, the one static setup page, from main/setup/*.
//
//   node tools/build_setup_page.mjs            write main/setup_page.html
//   node tools/build_setup_page.mjs --check    fail when the committed page is out of date
//
// The result is a single self-contained file (one <style>, one <script type="module">), because
// ES module imports do not work from file://. Its meta Content-Security-Policy allows only these two
// inline blocks by hash, so inline style attributes and event handlers could never run; the build
// refuses sources that contain them.
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const MODULES = ['transport_http.mjs', 'transport_serial.mjs', 'app.mjs']; // dependency order
const FRAME_GUARD =
  "if (window.top !== window.self) { document.documentElement.textContent = '请直接打开此页面'; throw 0; }";

export function readSources(directory = path.join(root, 'main/setup')) {
  const files = { 'page.html': '', 'style.css': '' };
  for (const name of [...Object.keys(files), ...MODULES])
    files[name] = fs.readFileSync(path.join(directory, name), 'utf8');
  return files;
}

function withoutComments(text) {
  return text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/.*$/gm, '');
}

// Everything the CSP would block at run time, caught at build time instead.
export function lintSources(files) {
  const problems = [];
  const rules = [
    [/<[A-Za-z][^<>]*\sstyle\s*=/, 'a style= attribute (the CSP allows no inline style)'],
    [/<[A-Za-z][^<>]*\son[a-z]+\s*=/i, 'an inline event handler such as onclick='],
    [/javascript\s*:/i, 'a javascript: URL'],
    [/setAttribute\(\s*['"](?:style|on[a-z]+)['"]/, 'setAttribute of style or an event handler'],
    [/(?:innerHTML|outerHTML|insertAdjacentHTML)[^;]*<(?:style|script)/i, 'markup with <style>'],
    [/document\.write/, 'document.write'],
  ];
  for (const [name, text] of Object.entries(files)) {
    const code = name.endsWith('.mjs') || name.endsWith('.css') ? withoutComments(text) : text;
    for (const [pattern, what] of rules) {
      if (pattern.test(code)) problems.push(`${name}: ${what}`);
    }
    if (name.endsWith('.mjs') && /<(?:style|script)\b/i.test(code))
      problems.push(`${name}: a <style> or <script> tag; styles live in style.css`);
  }
  const page = files['page.html'];
  const count = (pattern) => (page.match(pattern) ?? []).length;
  if (count(/<style>\/\*@STYLE\*\/<\/style>/g) !== 1) problems.push('page.html: one style slot');
  if (count(/<script type="module">\/\*@SCRIPT\*\/<\/script>/g) !== 1)
    problems.push('page.html: one module script slot');
  if (count(/<(?:script|style)\b/g) !== 2) problems.push('page.html: only the two inline blocks');
  if (count(/<!--@CSP-->/g) !== 1) problems.push('page.html: one CSP slot');
  if (/<(?:link|img|iframe|object|embed)\b/i.test(page))
    problems.push('page.html: an element that loads a resource');
  return problems;
}

// Join the modules into one script: drop imports and exports, keep order, refuse name clashes.
export function flattenModules(files) {
  const declared = new Map();
  const parts = MODULES.map((name) => {
    const text = files[name]
      .replace(/^import\s[\s\S]*?from\s+['"][^'"]+['"];?[ \t]*\n/gm, '')
      .replace(/^export\s+(?=(?:async\s+)?(?:function|class|const|let|var)\b)/gm, '');
    if (/^(?:import|export)\b/m.test(text))
      throw new Error(`${name}: unsupported import/export form`);
    for (const match of text.matchAll(
      /^(?:async\s+)?(?:function\*?|class|const|let|var)\s+([A-Za-z_$][\w$]*)/gm,
    )) {
      if (declared.has(match[1]))
        throw new Error(`${name}: ${match[1]} is already declared in ${declared.get(match[1])}`);
      declared.set(match[1], name);
    }
    return text.trim();
  });
  return parts.join('\n\n');
}

const hash = (text) => 'sha256-' + createHash('sha256').update(text, 'utf8').digest('base64');

// `preamble` is an extra classic script that runs first; the preview tool uses it to stand in
// for navigator.serial.
export function buildSetupPage({ files = readSources(), preamble = '' } = {}) {
  const problems = lintSources(files);
  if (problems.length)
    throw new Error('setup page sources are not CSP-clean:\n  ' + problems.join('\n  '));
  const style = `\n${files['style.css'].trim()}\n`;
  const script = `\n${FRAME_GUARD}\n${flattenModules(files)}\n`;
  for (const [what, text] of [
    ['script', script],
    ['preamble', preamble],
  ]) {
    if (/<\/script|<!--/i.test(text))
      throw new Error(`${what} contains a sequence that ends the script`);
  }
  if (/<\/style/i.test(style)) throw new Error('style contains </style');
  const scriptHashes = [hash(script), ...(preamble ? [hash(`\n${preamble}\n`)] : [])];
  const csp = [
    "default-src 'none'",
    `script-src ${scriptHashes.map((value) => `'${value}'`).join(' ')}`,
    `style-src '${hash(style)}'`,
    'img-src data:',
    "connect-src 'self'",
    "base-uri 'none'",
    "form-action 'none'",
  ].join('; ');
  const html = files['page.html']
    .replace('<!--@CSP-->', () => `<meta http-equiv="Content-Security-Policy" content="${csp}">`)
    .replace('<style>/*@STYLE*/</style>', () => `<style>${style}</style>`)
    .replace(
      '<script type="module">/*@SCRIPT*/</script>',
      () =>
        (preamble ? `<script>\n${preamble}\n</script>\n` : '') +
        `<script type="module">${script}</script>`,
    );
  return { html, csp };
}

const output = path.join(root, 'main/setup_page.html');

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const { html } = buildSetupPage();
    if (process.argv.includes('--check')) {
      const current = fs.existsSync(output) ? fs.readFileSync(output, 'utf8') : '';
      if (current !== html) {
        console.error('main/setup_page.html is out of date: run npm run build:setup');
        process.exit(1);
      }
      console.log('main/setup_page.html is up to date');
    } else {
      fs.writeFileSync(output, html);
      console.log(`wrote main/setup_page.html (${Buffer.byteLength(html)} bytes)`);
    }
  } catch (error) {
    console.error(error.message);
    process.exit(1);
  }
}
