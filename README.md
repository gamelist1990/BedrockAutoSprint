# BedrockAutoSprint

Minecraft Bedrock Windows x64向けAutoSprintサンプルMod。
[BedrockDynamicSDK](https://github.com/gamelist1990/BedrockDynamicSDK) はイベントの購読・発行APIと、必要な場合のシンボル解決を提供。
実際のスプリント入力、更新ループ、DLLエントリポイントはこの独立サンプル側で実装します。

## Event APIの使用例

イベント購読は src/auto_sprint.cpp に実装しています。

    keyId_ = events.on<bedrock::KeyEvent>([this](const auto& e) {
        if (e.virtualKey == 'W') forward_ = e.pressed;
    });
    updateId_ = events.on<bedrock::UpdateEvent>([this](const auto&) {
        update();
    });

- KeyEvent：キーコード・押下/解放・リピート
- GameplayEvent：ゲーム操作中の状態
- FocusEvent：ウィンドウフォーカス
- UpdateEvent：クライアント側で発行する更新通知
- on<T> / emit<T> / off<T>：購読 / 通知 / 解除

**UpdateEvent は Minecraft のゲームtickではありません。**
このサンプルのWin32アダプターが20msごとにイベントを発行します。
ネイティブのゲームTickや描画イベントは、別途検証済みのHookアダプターが必要です。
イベント購読と発行は単一の所有スレッドで扱います。

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
