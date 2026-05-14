/*
 * P2P Chat & File Transfer (Windows)
 * Компиляция (MinGW): gcc -o p2p.exe p2p.c -lws2_32
 * Запуск: p2p.exe
 */

#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <locale.h>
#include <sddl.h>

SECURITY_ATTRIBUTES* GetAllowAllSecurityAttributes() {
    static SECURITY_ATTRIBUTES sa;
    static BOOL initialized = FALSE;
    if (!initialized) {
        // NULL DACL — доступ разрешён всем
        PSECURITY_DESCRIPTOR pSD = NULL;
        if (ConvertStringSecurityDescriptorToSecurityDescriptor(
            L"D:(A;;GA;;;WD)",  // Allow Generic All to Everyone (WD)
            SDDL_REVISION_1,
            &pSD,
            NULL)) {
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.lpSecurityDescriptor = pSD;
            sa.bInheritHandle = FALSE;
        }
        initialized = TRUE;
    }
    return &sa;
}
#pragma comment(lib, "ws2_32.lib")

 // ======================== Настройки ========================
#define PIPE_NAME           "\\\\.\\pipe\\MyP2PChat"
#define MAILSLOT_NAME       "\\\\.\\mailslot\\MyP2PBroadcast"
#define BROADCAST_ADDR      "\\\\*\\mailslot\\MyP2PBroadcast"
#define BUFFER_SIZE         4096
#define LOG_FILE            "chat_log.txt"

// ======================== Глобальные переменные ========================
CRITICAL_SECTION logLock;         // Синхронизация записи в лог
HANDLE hServerThread = NULL;      // Поток сервера каналов
HANDLE hMailslotThread = NULL;    // Поток приёма широковещательных сообщений
HANDLE hCurrentPipe = INVALID_HANDLE_VALUE; // Текущее клиентское соединение
BOOL g_running = TRUE;            // Флаг работы программы

// ======================== Прототипы функций ========================
void WriteLog(const char* message);
void HandleCommand(char* cmdLine);
DWORD WINAPI ServerThread(LPVOID lpParam);
DWORD WINAPI ClientHandlerThread(LPVOID lpParam);
DWORD WINAPI MailslotReaderThread(LPVOID lpParam);

BOOL SendMessagePipe(HANDLE hPipe, const char* message);
BOOL ReceiveMessagePipe(HANDLE hPipe, char* buffer, DWORD bufferSize);
BOOL SendFileOverPipe(HANDLE hPipe, const char* filePath);
BOOL ReceiveFileOverPipe(HANDLE hPipe, const char* saveDir);
BOOL SendFileRequest(HANDLE hPipe, const char* remotePath);
BOOL ProcessRemoteCommand(HANDLE hPipe, const char* command);

// ======================== Точка входа ========================
int main() {
    InitializeCriticalSection(&logLock);

    setlocale(LC_ALL, "Russian");
    // Запуск серверной части (именованный канал)
    hServerThread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);
    if (hServerThread == NULL) {
        printf("Не удалось запустить сервер каналов.\n");
        return 1;
    }

    // Запуск слушателя широковещательных сообщений
    hMailslotThread = CreateThread(NULL, 0, MailslotReaderThread, NULL, 0, NULL);
    if (hMailslotThread == NULL) {
        printf("Не удалось запустить слушатель почтового ящика.\n");
        return 1;
    }

    printf("P2P узел запущен. Введите команду (/help для справки)\n");

    // Основной цикл обработки команд
    char cmdLine[512];
    while (g_running) {
        printf("> ");
        if (fgets(cmdLine, sizeof(cmdLine), stdin) == NULL) break;
        cmdLine[strcspn(cmdLine, "\n")] = 0; // Удаление \n
        if (strlen(cmdLine) > 0) {
            HandleCommand(cmdLine);
        }
    }

    // Корректное завершение
    g_running = FALSE;
    if (hCurrentPipe != INVALID_HANDLE_VALUE) CloseHandle(hCurrentPipe);
    WaitForSingleObject(hServerThread, 3000);
    WaitForSingleObject(hMailslotThread, 3000);
    DeleteCriticalSection(&logLock);
    return 0;
}

// ======================== Запись в лог ========================
void WriteLog(const char* message) {
    EnterCriticalSection(&logLock);
    FILE* f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s\n", message);
        fclose(f);
    }
    LeaveCriticalSection(&logLock);
}

// ======================== Обработка команд ========================
void HandleCommand(char* cmdLine) {
    char cmd[64] = { 0 }, arg1[256] = { 0 }, arg2[256] = { 0 };
    sscanf(cmdLine, "%63s %255s %255[^\n]", cmd, arg1, arg2);

    if (_stricmp(cmd, "/help") == 0) {
        printf("Команды:\n"
            "/connect <компьютер>     - Подключиться к удалённому узлу\n"
            "/msg <текст>             - Отправить сообщение в чате\n"
            "/sendfile <путь>         - Отправить файл собеседнику\n"
            "/getfile <комп> <файл> [сохр] - Получить файл с удалённого узла\n"
            "/broadcast <текст>       - Широковещательное сообщение\n"
            "/history                 - Просмотр истории\n"
            "/savehistory <файл>      - Сохранить историю\n"
            "/exit                    - Завершить текущий сеанс\n"
            "/quit                    - Выйти из программы\n");
    }
    else if (_stricmp(cmd, "/connect") == 0) {
        if (strlen(arg1) == 0) {
            printf("Укажите имя компьютера.\n");
            return;
        }
        // Закрываем предыдущее соединение, если есть
        if (hCurrentPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(hCurrentPipe);
            hCurrentPipe = INVALID_HANDLE_VALUE;
        }
        char pipeName[512];
        sprintf(pipeName, "\\\\%s\\pipe\\MyP2PChat", arg1);
        hCurrentPipe = CreateFile(pipeName, GENERIC_READ | GENERIC_WRITE, 0,NULL, OPEN_EXISTING, 0, NULL);
        if (hCurrentPipe == INVALID_HANDLE_VALUE) {
            printf("Не удалось подключиться к '%s'. Ошибка %d\n", arg1, GetLastError());
            return;
        }
        printf("Подключено к %s. Можете общаться (/msg, /sendfile, /exit)\n", arg1);

        // Поток для приёма сообщений от удалённой стороны
        HANDLE hRecvThread = (HANDLE)_beginthreadex(NULL, 0,
            [](void* param) -> unsigned {
                HANDLE hPipe = (HANDLE)param;
                char buffer[BUFFER_SIZE];
                while (g_running && hPipe != INVALID_HANDLE_VALUE) {
                    if (ReceiveMessagePipe(hPipe, buffer, BUFFER_SIZE)) {
                        printf("\r<< %s\n> ", buffer);
                        WriteLog(buffer);
                    }
                    else {
                        printf("\r[Соединение разорвано]\n> ");
                        break;
                    }
                }
                return 0;
            }, hCurrentPipe, 0, NULL);
        CloseHandle(hRecvThread); // Поток будет работать, пока жив канал
    }
    else if (_stricmp(cmd, "/msg") == 0) {
        if (hCurrentPipe == INVALID_HANDLE_VALUE) {
            printf("Сначала подключитесь к узлу (/connect).\n");
            return;
        }
        char message[BUFFER_SIZE];
        sprintf(message, "MSG:%s", arg1);
        if (SendMessagePipe(hCurrentPipe, message)) {
            printf(">> %s\n", arg1);
            WriteLog(message);
        }
        else {
            printf("Ошибка отправки.\n");
        }
    }
    else if (_stricmp(cmd, "/sendfile") == 0) {
        if (hCurrentPipe == INVALID_HANDLE_VALUE) {
            printf("Сначала подключитесь к узлу.\n");
            return;
        }
        if (strlen(arg1) == 0) {
            printf("Укажите путь к файлу.\n");
            return;
        }
        // Отправляем команду FILE, затем данные
        char header[512];
        char fileName[_MAX_FNAME];
        char ext[_MAX_EXT];
        _splitpath(arg1, NULL, NULL, fileName, ext);
        sprintf(header, "FILE:%s%s", fileName, ext);
        if (!SendMessagePipe(hCurrentPipe, header)) {
            printf("Ошибка отправки заголовка.\n");
            return;
        }
        if (!SendFileOverPipe(hCurrentPipe, arg1)) {
            printf("Ошибка отправки файла.\n");
        }
        else {
            printf("Файл отправлен.\n");
            WriteLog(header);
        }
    }
    else if (_stricmp(cmd, "/getfile") == 0) {
        if (strlen(arg1) == 0 || strlen(arg2) == 0) {
            printf("Использование: /getfile <компьютер> <удалённый_файл> [локальный_путь]\n");
            return;
        }
        // Подключаемся к удалённому узлу специально для запроса
        char pipeName[512];
        sprintf(pipeName, "\\\\%s\\pipe\\MyP2PChat", arg1);
        HANDLE hGetPipe = CreateFile(pipeName, GENERIC_READ | GENERIC_WRITE, 0,
            NULL, OPEN_EXISTING, 0, NULL);
        if (hGetPipe == INVALID_HANDLE_VALUE) {
            printf("Не удалось подключиться к '%s'.\n", arg1);
            return;
        }
        // Отправляем запрос GETFILE
        char request[512];
        sprintf(request, "GETFILE:%s", arg2);
        if (!SendMessagePipe(hGetPipe, request)) {
            printf("Ошибка запроса.\n");
            CloseHandle(hGetPipe);
            return;
        }
        // Принимаем файл
        char savePath[MAX_PATH];
        if (strlen(arg2) > 0) {
            strcpy(savePath, arg2);
        }
        else {
            char fname[_MAX_FNAME], ext[_MAX_EXT];
            _splitpath(arg2, NULL, NULL, fname, ext);
            sprintf(savePath, "%s%s", fname, ext);
        }
        if (ReceiveFileOverPipe(hGetPipe, savePath)) {
            printf("Файл сохранён как '%s'.\n", savePath);
            WriteLog("GETFILE success");
        }
        else {
            printf("Ошибка приёма файла.\n");
        }
        CloseHandle(hGetPipe);
    }
    else if (_stricmp(cmd, "/broadcast") == 0) {
        if (strlen(arg1) == 0) {
            printf("Введите сообщение для рассылки.\n");
            return;
        }
        HANDLE hMailslot = CreateFile(BROADCAST_ADDR, GENERIC_WRITE,
            FILE_SHARE_READ, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hMailslot == INVALID_HANDLE_VALUE) {
            printf("Не удалось открыть почтовый ящик для рассылки. Ошибка %d\n", GetLastError());
            return;
        }
        char message[BUFFER_SIZE];
        sprintf(message, "BCAST:%s", arg1);
        DWORD written;
        if (WriteFile(hMailslot, message, strlen(message) + 1, &written, NULL)) {
            printf("Широковещательное сообщение отправлено.\n");
            WriteLog(message);
        }
        else {
            printf("Ошибка отправки.\n");
        }
        CloseHandle(hMailslot);
    }
    else if (_stricmp(cmd, "/history") == 0) {
        FILE* f = fopen(LOG_FILE, "r");
        if (!f) {
            printf("История пуста или файл не найден.\n");
            return;
        }
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            printf("%s", line);
        }
        fclose(f);
    }
    else if (_stricmp(cmd, "/savehistory") == 0) {
        if (strlen(arg1) == 0) {
            printf("Укажите имя файла.\n");
            return;
        }
        if (CopyFile(LOG_FILE, arg1, FALSE)) {
            printf("История сохранена в '%s'.\n", arg1);
        }
        else {
            printf("Ошибка копирования.\n");
        }
    }
    else if (_stricmp(cmd, "/exit") == 0) {
        if (hCurrentPipe != INVALID_HANDLE_VALUE) {
            SendMessagePipe(hCurrentPipe, "EXIT");
            CloseHandle(hCurrentPipe);
            hCurrentPipe = INVALID_HANDLE_VALUE;
            printf("Сеанс завершён.\n");
        }
        else {
            g_running = FALSE; // Выход из программы, если нет активного сеанса
        }
    }
    else if (_stricmp(cmd, "/quit") == 0) {
        g_running = FALSE;
    }
    else {
        printf("Неизвестная команда. Введите /help.\n");
    }
}

// ======================== Сервер именованных каналов ========================
DWORD WINAPI ServerThread(LPVOID lpParam) {
    while (g_running) {
        HANDLE hPipe = CreateNamedPipe(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            BUFFER_SIZE, BUFFER_SIZE, 0,
            GetAllowAllSecurityAttributes());

        if (hPipe == INVALID_HANDLE_VALUE) {
            printf("Ошибка создания канала: %d\n", GetLastError());
            break;
        }

        BOOL connected = ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected) {
            printf("\r[Входящее подключение]\n> ");
            HANDLE hClientThread = CreateThread(NULL, 0, ClientHandlerThread, (LPVOID)hPipe, 0, NULL);
            if (hClientThread == NULL) {
                CloseHandle(hPipe);
            }
            else {
                CloseHandle(hClientThread);
            }
        }
        else {
            CloseHandle(hPipe);
        }
    }
    return 0;
}

// ======================== Обработчик клиента на сервере ========================
DWORD WINAPI ClientHandlerThread(LPVOID lpParam) {
    HANDLE hPipe = (HANDLE)lpParam;
    char buffer[BUFFER_SIZE];
    while (g_running) {
        if (!ReceiveMessagePipe(hPipe, buffer, BUFFER_SIZE)) {
            break;
        }
        printf("\r<< %s\n> ", buffer);
        WriteLog(buffer);

        // Анализируем команду
        if (strncmp(buffer, "MSG:", 4) == 0) {
            // Просто сообщение, уже выведено
        }
        else if (strncmp(buffer, "FILE:", 5) == 0) {
            // Принимаем файл
            char fileName[MAX_PATH];
            sscanf(buffer + 5, "%s", fileName);
            if (ReceiveFileOverPipe(hPipe, fileName)) {
                printf("\rФайл '%s' получен.\n> ", fileName);
            }
        }
        else if (strncmp(buffer, "GETFILE:", 8) == 0) {
            // Запрос файла от клиента – отправляем
            char remotePath[MAX_PATH];
            strcpy(remotePath, buffer + 8);
            // Отправляем заголовок FILE и файл
            char fileName[_MAX_FNAME], ext[_MAX_EXT];
            _splitpath(remotePath, NULL, NULL, fileName, ext);
            sprintf(buffer, "FILE:%s%s", fileName, ext);
            SendMessagePipe(hPipe, buffer);
            SendFileOverPipe(hPipe, remotePath);
        }
        else if (strcmp(buffer, "EXIT") == 0) {
            break;
        }
    }
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    printf("\r[Клиент отключился]\n> ");
    return 0;
}

// ======================== Широковещательный приёмник ========================
DWORD WINAPI MailslotReaderThread(LPVOID lpParam) {
    HANDLE hMailslot = CreateMailslot(MAILSLOT_NAME, 0, MAILSLOT_WAIT_FOREVER, NULL);
    if (hMailslot == INVALID_HANDLE_VALUE) {
        printf("Ошибка создания почтового ящика: %d\n", GetLastError());
        return 1;
    }
    char buffer[BUFFER_SIZE];
    DWORD bytesRead;
    while (g_running) {
        if (ReadFile(hMailslot, buffer, BUFFER_SIZE, &bytesRead, NULL)) {
            if (bytesRead > 0) {
                printf("\r[Broadcast] %s\n> ", buffer + 6); // убираем "BCAST:"
                WriteLog(buffer);
            }
        }
        else {
            // Ошибка чтения или ящик закрыт
        }
    }
    CloseHandle(hMailslot);
    return 0;
}

// ======================== Вспомогательные функции ========================
BOOL SendMessagePipe(HANDLE hPipe, const char* message) {
    DWORD written;
    BOOL res = WriteFile(hPipe, message, (DWORD)strlen(message) + 1, &written, NULL);
    return res;
}

BOOL ReceiveMessagePipe(HANDLE hPipe, char* buffer, DWORD bufferSize) {
    DWORD bytesRead;
    BOOL res = ReadFile(hPipe, buffer, bufferSize, &bytesRead, NULL);
    return res && bytesRead > 0;
}

BOOL SendFileOverPipe(HANDLE hPipe, const char* filePath) {
    HANDLE hFile = CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        // Отправим размер 0 как ошибку
        DWORD size = 0;
        WriteFile(hPipe, &size, sizeof(DWORD), NULL, NULL);
        return FALSE;
    }
    DWORD fileSize = GetFileSize(hFile, NULL);
    // Сначала отправляем размер файла
    WriteFile(hPipe, &fileSize, sizeof(DWORD), NULL, NULL);
    if (fileSize == 0) {
        CloseHandle(hFile);
        return TRUE;
    }
    // Отправляем содержимое порциями
    char buffer[BUFFER_SIZE];
    DWORD bytesRead, totalSent = 0;
    while (totalSent < fileSize) {
        if (!ReadFile(hFile, buffer, BUFFER_SIZE, &bytesRead, NULL) || bytesRead == 0) break;
        DWORD written;
        if (!WriteFile(hPipe, buffer, bytesRead, &written, NULL)) break;
        totalSent += written;
    }
    CloseHandle(hFile);
    return totalSent == fileSize;
}

BOOL ReceiveFileOverPipe(HANDLE hPipe, const char* savePath) {
    DWORD fileSize;
    if (!ReadFile(hPipe, &fileSize, sizeof(DWORD), NULL, NULL)) return FALSE;
    if (fileSize == 0) return FALSE; // Ошибка на удалённой стороне
    HANDLE hFile = CreateFile(savePath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;
    char buffer[BUFFER_SIZE];
    DWORD totalRead = 0, bytesRead;
    while (totalRead < fileSize) {
        DWORD toRead = min(BUFFER_SIZE, fileSize - totalRead);
        if (!ReadFile(hPipe, buffer, toRead, &bytesRead, NULL) || bytesRead == 0) {
            CloseHandle(hFile);
            return FALSE;
        }
        DWORD written;
        WriteFile(hFile, buffer, bytesRead, &written, NULL);
        totalRead += bytesRead;
    }
    CloseHandle(hFile);
    return TRUE;
}
