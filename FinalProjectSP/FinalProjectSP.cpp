#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <locale.h>
#include <sddl.h>

#define PIPE_NAME "\\\\.\\pipe\\LocalChatPipe"
#define MAILSLOT_NAME "\\\\.\\mailslot\\LocalChatMailslot"
#define BROADCAST_MAILSLOT "\\\\*\\mailslot\\LocalChatMailslot"
#define BUF_SIZE 4096

// Критическая секция для потокобезопасной записи в файл истории
CRITICAL_SECTION csHistory;

SECURITY_ATTRIBUTES* GetAllowAllSecurityAttributes() {
    static SECURITY_ATTRIBUTES sa;
    static BOOL initialized = FALSE;
    if (!initialized) {
        // NULL DACL — доступ разрешён всем
        PSECURITY_DESCRIPTOR pSD = NULL;
        if (ConvertStringSecurityDescriptorToSecurityDescriptor(
            "D:(A;;GA;;;WD)",  // Allow Generic All to Everyone (WD)
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

// Функция для сохранения переписки (Пункт 4)
void SaveToHistory(const char* sender, const char* message) {
    EnterCriticalSection(&csHistory);
    FILE* file = fopen("chat_history.txt", "a");
    if (file) {
        fprintf(file, "[%s]: %s\n", sender, message);
        fclose(file);
    }
    LeaveCriticalSection(&csHistory);
}

// ---------------- СЕРВЕРНАЯ ЧАСТЬ (Прием данных) ----------------

// Поток обработки конкретного клиента по именованному каналу (Пункт 6)
DWORD WINAPI ClientHandlerThread(LPVOID lpParam) {
    HANDLE hPipe = (HANDLE)lpParam;
    char buffer[BUF_SIZE];
    DWORD bytesRead, bytesWritten;

    while (ReadFile(hPipe, buffer, BUF_SIZE - 1, &bytesRead, NULL) && bytesRead != 0) {
        buffer[bytesRead] = '\0';
         
        // Обработка текстового сообщения 
        if (strncmp(buffer, "MSG:", 4) == 0) {
            printf("\n[Входящее сообщение]: %s\n", buffer + 4);
            SaveToHistory("Входящее", buffer + 4);

            // Ответ сервера (двусторонний обмен)
            const char* reply = "Сообщение доставлено.";
            WriteFile(hPipe, reply, strlen(reply), &bytesWritten, NULL);
        }
        // Обработка запроса на загрузку файла (Пункты 1 и 3)
        else if (strncmp(buffer, "FILE:", 5) == 0) {
            char filename[256] = { 0 };
            long filesize = 0;

            // Парсим имя и размер файла
            if (sscanf(buffer + 5, "%255[^:]:%ld", filename, &filesize) == 2) {
                printf("\n[Сервер] Входящий файл: %s (%ld байт). Загрузка...\n", filename, filesize);

                // Добавляем префикс recv_, чтобы не перезаписать локальный файл при тестировании на 1 ПК
                char savePath[512];
                sprintf(savePath, "recv_%s", filename);

                FILE* f = fopen(savePath, "wb");
                if (f) {
                    // Отправляем сигнал готовности к приему байтов
                    WriteFile(hPipe, "ACK", 3, &bytesWritten, NULL);

                    long totalRead = 0;
                    char fileBuf[BUF_SIZE];

                    // Читаем бинарные данные кусками
                    while (totalRead < filesize) {
                        DWORD toRead = (filesize - totalRead < BUF_SIZE) ? (DWORD)(filesize - totalRead) : BUF_SIZE;
                        if (ReadFile(hPipe, fileBuf, toRead, &bytesRead, NULL)) {
                            fwrite(fileBuf, 1, bytesRead, f);
                            totalRead += bytesRead;
                        }
                        else {
                            break;
                        }
                    }
                    fclose(f);
                    printf("\n[Сервер] Файл успешно сохранен как: %s\n> ", savePath);

                    const char* reply = "Файл успешно загружен на сервер.";
                    WriteFile(hPipe, reply, strlen(reply), &bytesWritten, NULL);
                }
                else {
                    WriteFile(hPipe, "ERR", 3, &bytesWritten, NULL);
                    printf("\n[Сервер] Ошибка создания файла %s\n> ", savePath);
                }
            }
        }
        else if (strcmp(buffer, "выход") == 0) {
            printf("\n[Сервер] Клиент отключился.\n> ");
            break;
        }
    }

    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
    return 0;
}

// Поток сервера Именованных каналов (ожидает подключения клиентов)
DWORD WINAPI PipeServerThread(LPVOID lpParam) {
    while (1) {
        HANDLE hPipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, // Пункт 6: несколько клиентов
            BUF_SIZE, BUF_SIZE,
            0, GetAllowAllSecurityAttributes());

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        if (ConnectNamedPipe(hPipe, NULL) != FALSE) {
            CreateThread(NULL, 0, ClientHandlerThread, (LPVOID)hPipe, 0, NULL);
        }
        else {
            CloseHandle(hPipe);
        }
    }
    return 0;
}

// Поток сервера Почтовых ящиков (Пункт 5: принимает Broadcast)
DWORD WINAPI MailslotServerThread(LPVOID lpParam) {
    HANDLE hSlot = CreateMailslotA(MAILSLOT_NAME, 0, MAILSLOT_WAIT_FOREVER, NULL);
    if (hSlot == INVALID_HANDLE_VALUE) return 1;

    DWORD msgSize, bytesRead;
    char buffer[BUF_SIZE];

    while (1) {
        if (GetMailslotInfo(hSlot, NULL, &msgSize, NULL, NULL) && msgSize != MAILSLOT_NO_MESSAGE) {
            if (ReadFile(hSlot, buffer, msgSize, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                printf("\n[ШИРОКОВЕЩАТЕЛЬНОЕ СООБЩЕНИЕ]: %s\n> ", buffer);
                SaveToHistory("Broadcast", buffer);
            }
        }
        else {
            Sleep(100);
        }
    }
    return 0;
}

// ---------------- КЛИЕНТСКАЯ ЧАСТЬ (Отправка данных) ----------------

// Подключение к серверу и двусторонний чат/файлы (Пункты 2, 3, 4)
void Client_ChatMode(const char* serverName) {
    char pipePath[256];
    sprintf(pipePath, "\\\\%s\\pipe\\LocalChatPipe", serverName);

    HANDLE hPipe;
    printf("Попытка подключения к %s...\n", serverName);

    while (TRUE) {
        hPipe = CreateFileA(pipePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe != INVALID_HANDLE_VALUE) break;

        if (GetLastError() != ERROR_PIPE_BUSY) {
            printf("Не удалось найти компьютер '%s' или сервер не запущен.\n", serverName);
            GetLastError();
            return;
        }
        WaitNamedPipeA(pipePath, 5000);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    char message[BUF_SIZE];
    char sendBuffer[BUF_SIZE];
    DWORD bytesWritten, bytesRead;

    printf("\nПодключено к %s!\n", serverName);
    printf("Пишите сообщения. Для отправки файла введите: /file ИМЯ_ФАЙЛА\n");
    printf("Для выхода введите: выход\n");

    while (TRUE) {
        printf("Вы: ");
        fgets(message, BUF_SIZE, stdin);
        message[strcspn(message, "\n")] = 0;

        if (strcmp(message, "выход") == 0) {
            WriteFile(hPipe, message, strlen(message), &bytesWritten, NULL);
            break;
        }

        // --- ЛОГИКА ОТПРАВКИ ФАЙЛА ---
        if (strncmp(message, "/file ", 6) == 0) {
            char* filename = message + 6;
            FILE* f = fopen(filename, "rb");
            if (!f) {
                printf("Ошибка: файл '%s' не найден локально.\n", filename);
                continue;
            }

            fseek(f, 0, SEEK_END);
            long filesize = ftell(f);
            fseek(f, 0, SEEK_SET);

            // Отправляем метаданные
            sprintf(sendBuffer, "FILE:%s:%ld", filename, filesize);
            WriteFile(hPipe, sendBuffer, strlen(sendBuffer), &bytesWritten, NULL);

            // Ждем готовность сервера (ACK)
            ReadFile(hPipe, sendBuffer, BUF_SIZE, &bytesRead, NULL);
            sendBuffer[bytesRead] = '\0';
            if (strcmp(sendBuffer, "ACK") != 0) {
                printf("Сервер отклонил передачу файла.\n");
                fclose(f);
                continue;
            }

            printf("Отправка файла (%ld байт)...\n", filesize);
            char fileBuf[BUF_SIZE];
            size_t bytesReadFromFile;

            // Отправляем куски бинарных данных
            while ((bytesReadFromFile = fread(fileBuf, 1, BUF_SIZE, f)) > 0) {
                WriteFile(hPipe, fileBuf, (DWORD)bytesReadFromFile, &bytesWritten, NULL);
            }
            fclose(f);

            // Читаем подтверждение о получении файла
            if (ReadFile(hPipe, sendBuffer, BUF_SIZE - 1, &bytesRead, NULL)) {
                sendBuffer[bytesRead] = '\0';
                printf("Сервер: %s\n", sendBuffer);
            }
            continue;
        }

        // --- ЛОГИКА ОТПРАВКИ СООБЩЕНИЯ ---
        sprintf(sendBuffer, "MSG:%s", message);
        SaveToHistory("Вы", message);

        WriteFile(hPipe, sendBuffer, strlen(sendBuffer), &bytesWritten, NULL);

        if (ReadFile(hPipe, sendBuffer, BUF_SIZE - 1, &bytesRead, NULL)) {
            sendBuffer[bytesRead] = '\0';
            printf("[%s]: %s\n", serverName, sendBuffer);
        }
    }
    CloseHandle(hPipe);
}

// Отправка бродкаст сообщения (Пункт 5)
void Client_BroadcastMsg(char text[128]) {
    HANDLE hFile = CreateFileA(BROADCAST_MAILSLOT, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("Ошибка открытия Mailslot для отправки Broadcast.\n");
        return;
    }
    char* message = text + 10;
    DWORD bytesWritten;
    //fgets(message, BUF_SIZE, stdin);
    message[strcspn(message, "\n")] = 0;

    WriteFile(hFile, message, strlen(message), &bytesWritten, NULL);
    CloseHandle(hFile);
    printf("Сообщение успешно отправлено всем компьютерам в домене/сети!\n");
}

int main() {
    // Включаем поддержку русского языка в консоли
    SetConsoleOutputCP(1251);
    SetConsoleCP(1251);

    InitializeCriticalSection(&csHistory);


    char myComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD nameSize = sizeof(myComputerName);
    GetComputerNameA(myComputerName, &nameSize);

    printf("=========================================\n");
    printf("Ваше имя компьютера в сети: %s\n", myComputerName);
    printf("Сообщите его собеседнику для подключения.\n");
    printf("=========================================\n");


    CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
    CreateThread(NULL, 0, MailslotServerThread, NULL, 0, NULL);

    char choice[128];
    char targetName[128];


    while (TRUE) {
        fgets(choice, sizeof(choice), stdin);
        if (strncmp(choice, "/connect ", 9) == 0) {
            char* targetName = choice + 9;
            targetName[strcspn(targetName, "\n")] = 0;

            if (strlen(targetName) > 0) {
                Client_ChatMode(targetName);
            }
            continue;
        }
        if (strncmp(choice, "/broadcast ", 10) == 0) {
            Client_BroadcastMsg(choice);
            continue;
        }
        if (strncmp(choice, "/history", 8) == 0) {
            EnterCriticalSection(&csHistory);
            FILE* f = fopen("chat_history.txt", "r");
            if (f) {
                char line[512];
                printf("\n--- ИСТОРИЯ ПЕРЕПИСКИ ---\n");
                while (fgets(line, sizeof(line), f)) printf("%s", line);
                fclose(f);
            }
            else {
                printf("История пока пуста.\n");
            }
            LeaveCriticalSection(&csHistory);
            continue;
        }
        if (strncmp(choice, "/exit", 5) == 0) {
            DeleteCriticalSection(&csHistory);
            exit(0);
        }

        if (strcmp(choice, "/h\n") == 0) {
            printf("\n--- Список команд ---\n");
            printf("/connect <Имя ПК> Подключиться к ПК по имени (Чат / Файлы)\n");
            printf("/broadcast <сообщение> Отправить сообщение ВСЕМ в сети\n");
            printf("/history Просмотреть историю переписки\n");
            printf("/exit Выйти из приложения\n");
            printf("============================================================\n");
            continue;
        }
        printf("не найдена команда %s", choice);
        printf("Для справки введите /h\n");
    }

    DeleteCriticalSection(&csHistory);
    return 0;
}