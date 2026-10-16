# BedrockAutoSprint

**Minecraft Bedrock Windows x64 / AutoSprintだけのサンプルMod**

[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) のSDKと
DLLサンプルを**別々のリポジトリ・CMakeプロジェクト**として管理する例です。
このプロジェクトにはBedrock SDKのヘッダーやソースコードの複製は含めません。

- 実行ファイル：`BedrockAutoSprint.dll` だけ
- SDK取得方法：CMake **FetchContent** がGitHubの `BedrockDynamicSDK` を取得
- 使用するSDK：`<bedrock/remote_catalog.hpp>`, `<bedrock/resolver.hpp>` と `bedrock_resolver`
- PreLoader/LeviLamina/DLL追加依存なし
- ゲーム内GUIなし、設定ファイルなし

## 実装する機能

**Wキーで前進している間、自動的に左Ctrlを押してスプリント**。
Wを離す・画面を離れる・メニューを開く・Shiftを押すと解放します。
ゲームの移動速度を書き換えたり、スプリント状態のメモリを直接変更したりはしません。
既定キー配置（Wと左Ctrl）向け。キー割当の変更やコントローラーは未対応。

## 動的バージョン対応

DLLをロードすると、SDKが以下を実行します。

1. 実際にロードされた `Minecraft.Windows.exe` のSHA256とPE構造を確認。
2. GitHubの[manifest.json](https://github.com/gamelist1990/BedrockDynamicSDK/blob/main/data/manifest.json)をHTTPSで取得。
3. 対応バージョンをハッシュとPEレイアウトの**完全一致**で選択。
4. 同一GitHubリポジトリの `data/<version>/windows-x64.json` を取得。
5. JSONのSHA256、対象ゲームのSHA256、必要なClientInput関数候補を検証。
6. すべて成功した場合だけAutoSprint開始。**未対応・オフライン・不一致は何もしない。**

対応バージョンのJSONを追加することで、同じWindowsキー入力方式に対して
DLLを更新せずに新バージョンを認識できます。内部ABIの互換性まで保証するものではありません。

## ビルド

Windows x64 + Visual Studio 2022（C++/CMake）：

```powershell
git clone https://github.com/gamelist1990/BedrockAutoSprint.git
cd BedrockAutoSprint

cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

成果物：`build/Release/BedrockAutoSprint.dll`

**ビルド時にSDKをGitHubから取得**します。通常は `main` を取得。
互換性を確保したい場合はコミットIDまたはタグで固定：

```powershell
cmake -S . -B build -A x64 -DBEDROCK_SDK_REF=<git-tag-or-commit>
```

GitHub ActionsはWindows x64のDLLを生成し、ダウンロードできるアーティファクトとして公開します。
LinuxではAutoSprintの入力ロジックとSDKへのリンクを検証します。

## CMakeでのSDK取り込み

SDKの公開型は**ビルド中に** `BedrockDynamicSDK/include/` から取り込まれます。

```cmake
include(FetchContent)
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  bedrock_sdk
  GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
  GIT_TAG main
)
FetchContent_MakeAvailable(bedrock_sdk)
target_link_libraries(BedrockAutoSprint PRIVATE bedrock_resolver)
```

配布DLLにはSDKの静的ライブラリが統合されます。
Github上でのSDK追加・修正は**次回ビルド時に**反映でき、
ゲーム更新時のシンボルJSONは**DLL実行時に**取得します（別のタイミングです）。

## 注意

- このDLLは別のプロセスへの**注入機能を内蔵しません**。正規のDLLローダー等から
  自分でロードする必要があります。
- Minecraft本体への注入・動作試験はまだ行っていません。
- フックを設置しないため、クライアントの未検証オフセットやvtableは使いません。
- コード本体：`src/auto_sprint.cpp`。判断ロジック：`include/auto_sprint_policy.hpp`。
