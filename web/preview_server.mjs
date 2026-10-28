// 本地预览服务器：把 vite 产出的外壳（frontend/）与 atlas 媒体（web/public/atlas）
// 合并在同一个源下，行为和打包后的 exe 一致（含 Range 分段响应，音频可拖动）。
// 仅用于开发验证，不参与打包。
import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const shellRoot = path.resolve(here, "..", "frontend");
const atlasRoot = path.resolve(here, "public", "atlas");
const port = Number(process.env.PREVIEW_PORT || 4173);

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".mjs": "application/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".webp": "image/webp",
  ".ico": "image/x-icon",
  ".woff2": "font/woff2",
  ".mp3": "audio/mpeg",
  ".webmanifest": "application/manifest+json",
};

function mimeOf(file) {
  return MIME[path.extname(file).toLowerCase()] || "application/octet-stream";
}

function insideRoot(abs, root) {
  const rel = path.relative(root, abs);
  return rel !== "" && !rel.startsWith("..") && !path.isAbsolute(rel);
}

function resolveFile(urlPath) {
  const clean = decodeURIComponent(urlPath.split("?")[0].split("#")[0]);
  const candidates = [];

  if (clean.startsWith("/atlas/")) {
    candidates.push([path.join(atlasRoot, clean.slice("/atlas/".length)), atlasRoot]);
  }
  candidates.push([path.join(shellRoot, clean), shellRoot]);

  for (const [candidate, root] of candidates) {
    const abs = path.resolve(candidate);
    if (!insideRoot(abs, root) && abs !== path.resolve(shellRoot)) continue;
    if (fs.existsSync(abs) && fs.statSync(abs).isFile()) return abs;
  }
  return null;
}

const server = http.createServer((req, res) => {
  const urlPath = req.url || "/";
  let file = resolveFile(urlPath);

  // 单页应用回退
  if (!file && !path.extname(urlPath.split("?")[0])) {
    file = path.join(shellRoot, "index.html");
  }
  if (!file) {
    res.writeHead(404, { "Content-Type": "text/plain; charset=utf-8" });
    res.end("404");
    console.log(`${res.statusCode} ${urlPath}`);
    return;
  }

  const size = fs.statSync(file).size;
  const headers = {
    "Content-Type": mimeOf(file),
    "Accept-Ranges": "bytes",
    "Access-Control-Allow-Origin": "*",
    "Cache-Control": "no-cache",
  };

  const range = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || "");
  if (range) {
    const start = range[1] ? Number(range[1]) : 0;
    const end = range[2] ? Math.min(Number(range[2]), size - 1) : size - 1;
    if (start >= size || end < start) {
      res.writeHead(416, { ...headers, "Content-Range": `bytes */${size}` });
      res.end();
      console.log(`416 ${urlPath}`);
      return;
    }
    res.writeHead(206, {
      ...headers,
      "Content-Range": `bytes ${start}-${end}/${size}`,
      "Content-Length": end - start + 1,
    });
    fs.createReadStream(file, { start, end }).pipe(res);
    console.log(`206 ${urlPath} ${start}-${end}/${size}`);
    return;
  }

  res.writeHead(200, { ...headers, "Content-Length": size });
  fs.createReadStream(file).pipe(res);
  console.log(`200 ${urlPath} (${size}B)`);
});

server.listen(port, "127.0.0.1", () => {
  console.log(`preview: http://127.0.0.1:${port}/  shell=${shellRoot}  atlas=${atlasRoot}`);
});
