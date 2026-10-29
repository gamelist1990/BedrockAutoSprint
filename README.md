# BedrockAutoSprint

Windows配布物は `BedrockAutoSprint.dll` 単体です。SDK hook runtimeとMinHookはDLL内に静的同封され、他のSDK製Modとはプロセス内でHook所有権を共有します。
SDK共有ランタイムが他のBedrockDynamicSDK対応ModとMinecraft hookを共用します。

Minecraft Bedrock Windows x64向けAutoSprintサンプルMod。
[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) はイベントの購読・発行APIと、必要な場合のシンボル解決を提供。
実際のスプリント入力、更新ループ、DLLエントリポイントはこの独立サンプル側で実装します。

## Minecraftネイティブ入力

Minecraftの検証済み `GameCore::keyboardEvent` からキー遷移を受け取り、
SDKの `NativeKeyboardInput` を通して実際の `HIDController::onKeyDown/onKeyUp`
でCtrlを押下・解放します。`Keyboard::feed` はキーイベントの事前処理で、
それだけを呼んでも移動入力になりません。OSのキー状態ポーリングや
`SendInput` は使わず、ゲームプレイ中にWを押している間だけ自動スプリントします。

`InputFrameEvent` はサンプル内部で状態を評価するためのイベント型です。
イベント発行元は別スレッドのポーリングループではなく、Minecraftのキー入力と
カーソル取得/解放フックです。

```cpp
using AutoSprintEvents = bedrock::EventBus<InputFrameEvent>;

events.on<InputFrameEvent>([this](const InputFrameEvent& frame) {
    if (frame.shouldSprint() && !ownSprint_ && !frame.leftCtrlHeld) {
        if (send_(true)) ownSprint_ = true;
    } else if (!frame.shouldSprint()) {
        release();
    }
});

// Minecraft native keyboard callback とカーソル状態フックから発行
events.emit(InputFrameEvent{
    gameplayActive, gameplayActive, forwardHeld, shiftHeld, leftCtrlHeld
});
```

**Event API自体は軽量なヘッダーオンリー**で、入力の監視とスプリントの実行は
完全にサンプル側にあります。キーの押しっぱなし状態はネイティブイベントから追跡し、
生成したCtrlは物理入力と区別します。コールバックの配置と仮想関数slotは対応版の
JSONから読みます。古いキャッシュに必要な定義がない場合はDLL内の完全一致定義を
先に試すため、同じ版ならGitHubの再取得を待たずに起動できます。

カーソルを取得してゲームプレイへ戻ったときは、以後のMinecraft入力イベントから
状態を追跡します。入力イベントが発生しない間はバックグラウンドでポーリングしません。

## ファイル構成

    src/auto_sprint.cpp       AutoSprintイベント購読とスプリント判定
    src/win32_client.cpp      DLL起動、バージョン認識、Minecraft入力フック
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

AutoSprintは symbols を使い、ゲーム実行ファイルのハッシュが一致する検証済み
プロファイルだけで動作します。プロファイルは `%LOCALAPPDATA%\PEXData\BedrockDynamicSDK\profile-cache`
でTabList、Zoom、MouseChatterと共有し、キャッシュ／DLL内蔵データを優先します。
GitHubの再確認は最大1日1回で、ローカルに一致するデータがない新バージョンだけ初回取得します。
対応プロファイルが見つからない場合は何も変更しません。

## AutoSprint

- 既定キーWで前進、ゲームが前面かつ操作中に左Ctrlを自動で保持
- Wを離す、Shiftを押す、メニューを開く等で解除
- 移動速度・プレイヤー内部メモリを直接変更しない
- Minecraftのネイティブ `HIDController::onKeyDown/onKeyUp` を使用。カスタムキーとコントローラーには未対応
- 診断ログ：`Documents/PEXData/AutoSprint/AutoSprint.log`（起動時に前回履歴をクリア）

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

The sample uses SDK's optional header-only [Logger API](https://github.com/gamelist1990/BedrockDynamicSDK/blob/main/include/bedrock/logging/logger.hpp)
for startup/version verification messages, without opening another window by default.
On Windows, the debugger can receive OutputDebugStringA messages.

To show a console explicitly, build with:

    cmake -S . -B build -A x64 -DBEDROCK_AUTOSPRINT_DEBUG_CONSOLE=ON
    cmake --build build --config Release

The sample then calls Console::open(L"Bedrock AutoSprint Debug") on the worker
thread **after** DllMain. Any other mod can use:

    #include <bedrock/logging/console.hpp>
    #include <bedrock/logging/logger.hpp>

    bedrock::Console::open(L"My Debug Console");
    bedrock::Logger logger{"MyMod"};
    logger.debug("Loading...");
    logger.info("Loaded version: ", "1.26.52.3");
    logger.warn("Missing symbol: ", "ClientInstance");
    logger.error("Could not initialize");

For mod projects that only want logging, CMake target bedrock::logging is
header-only with no symbol resolver or networking dependency. The SDK never
opens a console automatically.
