#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
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
#define PIPE_NAME "\\\\.\\pipe\\LocalChatPipe"
#define MAILSLOT_NAME "\\\\.\\mailslot\\LocalChatMailslot"
#define BROADCAST_MAILSLOT "\\\\*\\mailslot\\LocalChatMailslot"
#define BUF_SIZE 1024

// Функция для сохранения переписки (Пункт 4)
void SaveToHistory(const char* sender, const char* message) {
    FILE* file = fopen("chat_history.txt", "a");
    if (file) {
        fprintf(file, "[%s]: %s\n", sender, message);
        fclose(file);
    }
}

// ---------------- СЕРВЕРНАЯ ЧАСТЬ ----------------

// Поток обработки конкретного клиента по именованному каналу (Пункт 6 - многопоточность)
DWORD WINAPI ClientHandlerThread(LPVOID lpParam) {
    HANDLE hPipe = (HANDLE)lpParam;
    char buffer[BUF_SIZE];
    DWORD bytesRead, bytesWritten;

    while (ReadFile(hPipe, buffer, BUF_SIZE - 1, &bytesRead, NULL) && bytesRead != 0) {
        buffer[bytesRead] = '\0';

        // Обработка команд протокола
        if (strncmp(buffer, "MSG:", 4) == 0) {
            printf("\n[Личное сообщение]: %s\n", buffer + 4);
            SaveToHistory("Client", buffer + 4);

            // Ответ сервера (двусторонний обмен)
            const char* reply = "Сообщение получено сервером.";
            WriteFile(hPipe, reply, strlen(reply), &bytesWritten, NULL);
        }
        else if (strncmp(buffer, "FILE_UPLOAD:", 12) == 0) {
            // Пункты 1 и 3: Здесь должна быть логика приема байтов и записи в fopen(filename, "wb")
            printf("\n[Запрос на загрузку файла]: %s\n", buffer + 12);
        }
        else if (strcmp(buffer, "выход") == 0) {
            printf("\nКлиент завершил сеанс.\n");
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
            0, NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }

        if (ConnectNamedPipe(hPipe, NULL) != FALSE) {
            // Создаем поток для клиента
            CreateThread(NULL, 0, ClientHandlerThread, (LPVOID)hPipe, 0, NULL);
        }
        else {
            CloseHandle(hPipe);
        }
    }
    return 0;
}

// Поток сервера Почтовых ящиков (Пункт 5: принимает сообщения от всех)
DWORD WINAPI MailslotServerThread(LPVOID lpParam) {
    HANDLE hSlot = CreateMailslotA(MAILSLOT_NAME, 0, MAILSLOT_WAIT_FOREVER, NULL);
    if (hSlot == INVALID_HANDLE_VALUE) return 1;

    DWORD msgSize, bytesRead;
    char buffer[BUF_SIZE];

    while (1) {
        if (GetMailslotInfo(hSlot, NULL, &msgSize, NULL, NULL) && msgSize != MAILSLOT_NO_MESSAGE) {
            if (ReadFile(hSlot, buffer, msgSize, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                printf("\n[БРОДКАСТ ВСЕМ]: %s\n", buffer);
                SaveToHistory("Broadcast", buffer);
            }
        }
        else {
            Sleep(100);
        }
    }
    return 0;
}

// ---------------- КЛИЕНТСКАЯ ЧАСТЬ ----------------

// Подключение к серверу и двусторонний чат (Пункты 2, 4)
void Client_ChatMode(const char* serverIP) {
    char pipePath[256];
    sprintf(pipePath, "\\\\%s\\pipe\\LocalChatPipe", serverIP);

    HANDLE hPipe;
    while (1) {
        hPipe = CreateFileA(pipePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, GetAllowAllSecurityAttributes());
        if (hPipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) {
            printf("Не удалось подключиться к серверу %s\n", serverIP);
            return;
        }
        WaitNamedPipeA(pipePath, 5000);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    char message[BUF_SIZE];
    char sendBuffer[BUF_SIZE];
    DWORD bytesWritten, bytesRead;

    printf("Подключено! Введите 'выход' для отключения.\n");
    while (1) {
        printf("Вы: ");
        fgets(message, BUF_SIZE, stdin);
        message[strcspn(message, "\n")] = 0; // Удаляем newline

        if (strcmp(message, "выход") == 0) {
            WriteFile(hPipe, message, strlen(message), &bytesWritten, NULL);
            break;
        }

        sprintf(sendBuffer, "MSG:%s", message);
        SaveToHistory("You", message);

        // Отправка
        WriteFile(hPipe, sendBuffer, strlen(sendBuffer), &bytesWritten, NULL);

        // Получение ответа
        if (ReadFile(hPipe, sendBuffer, BUF_SIZE - 1, &bytesRead, NULL)) {
            sendBuffer[bytesRead] = '\0';
            printf("Сервер: %s\n", sendBuffer);
        }
    }
    CloseHandle(hPipe);
}

// Отправка бродкаст сообщения (Пункт 5)
void Client_BroadcastMsg() {
    HANDLE hFile = CreateFileA(BROADCAST_MAILSLOT, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        printf("Ошибка отправки Broadcast.\n");
        return;
    }

    char message[BUF_SIZE];
    DWORD bytesWritten;
    printf("Введите широковещательное сообщение: ");
    fgets(message, BUF_SIZE, stdin);
    message[strcspn(message, "\n")] = 0;

    WriteFile(hFile, message, strlen(message), &bytesWritten, NULL);
    CloseHandle(hFile);
    printf("Сообщение отправлено всем компьютерам в сети!\n");
}

int main() {
    SetConsoleOutputCP(1251);
    SetConsoleCP(1251);

    // Запускаем серверные потоки
    CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
    CreateThread(NULL, 0, MailslotServerThread, NULL, 0, NULL);

    int choice;
    char ip[64];

    while (1) {
        printf("\n--- МЕНЮ ---\n");
        printf("1. Подключиться к другому ПК (Чат)\n");
        printf("2. Отправить сообщение ВСЕМ в сети (Mailslot)\n");
        printf("3. Просмотреть историю переписки\n");
        printf("0. Выйти из приложения\n");
        printf("Ваш выбор: ");
        if (scanf("%d", &choice) != 1) break;
        while (getchar() != '\n'); // Очистка буфера

        switch (choice) {
        case 1:
            printf("Введите IP компьютера (или '.' для локального): ");
            fgets(ip, sizeof(ip), stdin);
            ip[strcspn(ip, "\n")] = 0;
            Client_ChatMode(ip);
            break;
        case 2:
            Client_BroadcastMsg();
            break;
        case 3: {
            FILE* f = fopen("chat_history.txt", "r");
            if (f) {
                char line[256];
                printf("\n--- ИСТОРИЯ ---\n");
                while (fgets(line, sizeof(line), f)) printf("%s", line);
                fclose(f);
            }
            else {
                printf("История пуста.\n");
            }
            break;
        }
        case 0:
            exit(0);
        default:
            printf("Неверный выбор.\n");
        }
    }
    return 0;
}