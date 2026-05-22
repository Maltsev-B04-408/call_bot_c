# Call Bot C++

Проект представляет собой консольного торгового бота на C++. Бот читает сообщения из заданного Telegram-канала, пытается найти в них адрес токена Solana, покупает токен через Jupiter API, затем получает через Solana RPC фактическое количество купленных токенов и продает его обратно. Информация о покупке, продаже и ошибках отправляется в отдельную Telegram-группу.

Проект сделан как учебная работа по C++: основная логика написана на C++, используется ООП, STL, CMake и несколько сторонних библиотек для работы с Telegram, HTTP, JSON, криптографией и Solana/Jupiter API.

## Возможности

- авторизация в Telegram через TDLib;
- чтение новых сообщений из указанного Telegram-канала;
- извлечение адреса токена из текста, DexScreener, pump.fun и Photon-ссылок;
- получение котировки через Jupiter;
- создание, подпись и отправка Solana-транзакции;
- получение информации о покупке через Solana RPC;
- автоматическая продажа 100% купленных токенов;
- отправка отчетов о покупке и продаже в Telegram-группу;
- хранение приватных данных в локальном `.env`, который не попадает в git.

## Структура проекта

```text
src/
  main.cpp              основная логика бота, конфигурация, обработка сигналов
  TelegramClient.*      работа с Telegram через TDLib
  JupiterClient.*       запросы quote/swap к Jupiter API
  SolanaRpcClient.*     работа с Solana JSON-RPC
CMakeLists.txt          сборка проекта
.env.example            пример локальных настроек
```

## Зависимости


Нужны:

- CMake 3.10 или новее;
- компилятор C++17, например MSVC;
- TDLib;
- vcpkg;
- CURL;
- OpenSSL;
- libsodium;
- nlohmann_json.

В текущем `CMakeLists.txt` ожидаются такие пути:

```text
C:/tdlib/install
C:/vcpkg
```

Если библиотеки установлены в другие папки, пути нужно поменять в `CMakeLists.txt` или передать их через настройки CMake.

## Настройка

Перед запуском нужно создать файл `.env` в корне проекта. Удобнее всего взять за основу `.env.example`.

Пример:

```env
CALL_BOT_PRIVATE_KEY_BASE58=your_base58_private_key
CALL_BOT_WALLET_ADDRESS=your_wallet_address
CALL_BOT_TELEGRAM_API_ID=123456
CALL_BOT_TELEGRAM_API_HASH=your_telegram_api_hash
CALL_BOT_TRANSACTIONS_CHAT_ID=-5225436051
CALL_BOT_LISTEN_CHANNELS=-1003939814952

CALL_BOT_BUY_SOL_LAMPORTS=5000000

CALL_BOT_SOL_MINT=So11111111111111111111111111111111111111112
CALL_BOT_JUPITER_API_BASE=https://lite-api.jup.ag/swap/v1
CALL_BOT_SOLANA_RPC_ENDPOINT=https://api.mainnet-beta.solana.com
CALL_BOT_TELEGRAM_SESSION_DIR=telegram_session
```

`CALL_BOT_BUY_SOL_LAMPORTS=5000000` означает покупку на `0.005 SOL`.

Файл `.env` не должен попадать в репозиторий, потому что в нем лежат приватный ключ и данные Telegram API.

## Сборка

Из корня проекта:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

Для Release-сборки:

```powershell
cmake --build build --config Release
```



## Запуск

```powershell
.\build\Release\inflbot.exe
```

При первом запуске TDLib попросит авторизоваться в Telegram:

```text
Enter phone number:
Enter code:
Enter 2FA password:
```

После успешной авторизации сессия сохранится в папке `telegram_session`. Эта папка игнорируется git.

## Как работает бот

1. Бот подключается к Telegram через TDLib.
2. Загружает список каналов из `CALL_BOT_LISTEN_CHANNELS`.
3. При появлении нового сообщения проверяет, есть ли в нем адрес токена или поддерживаемая ссылка.
4. Получает quote через Jupiter.
5. Получает swap transaction от Jupiter.
6. Подписывает транзакцию локальным приватным ключом.
7. Отправляет транзакцию через Solana RPC.
8. Сразу отправляет сообщение о покупке в Telegram-группу.
9. Через RPC получает фактическое количество купленных токенов.
10. Продает 100% купленного количества.
11. Отправляет сообщение о продаже в Telegram-группу.

## Безопасность

Для работы используется приватный ключ Solana-кошелька. Его нельзя хранить прямо в коде или отправлять в репозиторий. Поэтому ключ вынесен в `.env`, а сам `.env` добавлен в `.gitignore`.

Для тестирования лучше использовать отдельный кошелек с небольшой суммой.
