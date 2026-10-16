# BedrockAutoSprint

Minecraft Bedrock Windows x64向けAutoSprintサンプルMod。
[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) はイベントの購読・発行APIと、必要な場合のシンボル解決を提供。
実際のスプリント入力、更新ループ、DLLエントリポイントはこの独立サンプル側で実装します。

## Event APIの使用例（入力スナップショット方式）

以前正常動作していた実装と同じく、毎回 `GetAsyncKeyState` と
フォーカス・カーソル状態を直接読み取り、**1フレーム分の状態を1つのイベント**
として流します。押下・解放の変化だけを追う実装は廃止しました。

`InputFrameEvent` はこのサンプル独自のイベント型です。
SDK本体にはAutoSprint依存を追加しません。

```cpp
using AutoSprintEvents = bedrock::EventBus<InputFrameEvent>;

events.on<InputFrameEvent>([this](const InputFrameEvent& frame) {
    if (frame.shouldSprint() && !ownSprint_ && !frame.leftCtrlHeld) {
        if (send_(true)) ownSprint_ = true;
    } else if (!frame.shouldSprint()) {
        release();
    }
});

// Win32アダプターは20msごとに現在の全入力状態を発行
events.emit(InputFrameEvent{
    minecraftFocused(), gameplayCursorHidden(),
    keyHeld('W'), keyHeld(VK_LSHIFT) || keyHeld(VK_RSHIFT),
    keyHeld(VK_LCONTROL)
});
```

**Event API自体は軽量なヘッダーオンリー**で、入力の監視とスプリントの実行は
完全にサンプル側にあります。Win32アダプターはMinecraftネイティブの
`TickEvent` を使うわけではありません。

フォーカス復帰やキーを押したままModをロードした場合でも、
スナップショットから即座に正しい状態に復帰できます。

## ファイル構成

    src/auto_sprint.cpp       AutoSprintイベント購読とスプリント判定
    src/win32_client.cpp      DLL起動、バージョン認識、Win32イベント発行
    include/auto_sprint_feature.hpp
    tests/auto_sprint_test.cpp
    CMakeLists.txt

## CMake経由でGitHubからSDKを取得

FetchContentを使い、クライアント内にSDKヘッダーを複製しません。

    include(FetchContent)
    FetchContent_Declare(
        bedrock_sdk
        GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(bedrock_sdk)

    target_link_libraries(MyMod PRIVATE bedrock::events)

bedrock::events はヘッダーオンリーで、WinHTTP/JSON/MinHook等のライブラリ依存なし。
ゲーム版の照合が必要なら、オプションとして次をリンク：

    target_link_libraries(MyMod PRIVATE bedrock::symbols)

AutoSprintは symbols を使い、GitHub manifest.json からゲーム版に完全一致した
data/<version>/windows-x64.jsonを取得できたときだけ動作します。
ネットワーク障害・非対応版は何も変更しません。

## AutoSprint

- 既定キーWで前進、ゲームが前面かつ操作中に左Ctrlを自動で保持
- Wを離す、Shiftを押す、メニューを開く等で解除
- 移動速度・プレイヤー内部メモリを直接変更しない
- Windows SendInputを使う。カスタムキーとコントローラーには未対応

## ビルド

Windows MSVC + CMake：

    git clone https://github.com/gamelist1990/BedrockAutoSprint.git
    cd BedrockAutoSprint
    cmake -S . -B build -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

成果物：build/Release/BedrockAutoSprint.dll（単一DLL）。
LinuxではイベントAPI・Modロジックのユニットテストを実施。
**Minecraft実機へのInject・ゲーム内動作テストは未実施**です。


## Debug console and Logger

The sample uses SDK's optional header-only [Logger API](https://github.com/gamelist1990/BedrockDynamicSDK/blob/main/include/bedrock/logger.hpp)
for startup/version verification messages, without opening another window by default.
On Windows, the debugger can receive OutputDebugStringA messages.

To show a console explicitly, build with:

    cmake -S . -B build -A x64 -DBEDROCK_AUTOSPRINT_DEBUG_CONSOLE=ON
    cmake --build build --config Release

The sample then calls Console::open(L"Bedrock AutoSprint Debug") on the worker
thread **after** DllMain. Any other mod can use:

    #include <bedrock/console.hpp>
    #include <bedrock/logger.hpp>

    bedrock::Console::open(L"My Debug Console");
    bedrock::Logger logger{"MyMod"};
    logger.debug("Loading...");
    logger.info("Loaded version: ", "1.26.52.3");
    logger.warn("Missing symbol: ", "ClientInstance");
    logger.error("Could not initialize");

For mod projects that only want logging, CMake target bedrock::logging is
header-only with no symbol resolver or networking dependency. The SDK never
opens a console automatically.
