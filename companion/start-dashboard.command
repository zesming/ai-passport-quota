#!/bin/zsh
set -e
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
aiq_app_directory="$(cd -- "$(dirname -- "$0")" && pwd)"
cd -- "$aiq_app_directory"
if ! command -v node >/dev/null 2>&1; then
  print '未找到 Node.js，请联系开发者完成运行环境配置。'
  read -r 'aiq_wait?按回车关闭'
  exit 1
fi
if [[ ! -f dist/client/index.html ]]; then
  print '未找到已编译的网页，请先完成应用构建。'
  read -r 'aiq_wait?按回车关闭'
  exit 1
fi
if curl --fail --silent --max-time 2 http://127.0.0.1:4317/ >/dev/null; then
  print '本地应用已经运行，请打开 http://127.0.0.1:4317/'
  read -r 'aiq_wait?按回车关闭'
  exit 0
fi
print '运行期间请保持此窗口打开；访问 http://127.0.0.1:4317/'
exec node server/index.mjs
