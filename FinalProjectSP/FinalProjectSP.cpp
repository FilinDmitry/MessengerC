#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <process.h>

#define PIPE_NAME "\\\\.\\pipe\\LocalChatPipe"
#define MAILSLOT_NAME "\\\\.\\mailslot\\LocalChatMailslot"
#define BROADCAST_MAILSLOT "\\\\*\\mailslot\\LocalChatMailslot"
#define BUF_SIZE 1024

// ... (здесь остаются функции SaveToHistory, ClientHandlerThread, PipeServerThread, MailslotServerThread из предыдущего ответа) ...

// Подключение к серверу по ИМЕНИ КОМПЬЮТЕРА (Пункты 2, 4)
void Client_ChatMode(const char* serverName) {
    char pipePath[256];

    // Формируем путь: \\ИМЯ_КОМПЬЮТЕРА\pipe\LocalChatPipe
    sprintf(pipePath, "\\\\%s\\pipe\\LocalChatPipe", serverName);

    HANDLE hPipe;
    printf("Попытка подключения к %s...\n", serverName);

    while (1) {
        hPipe = CreateFileA(pipePath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPipe != INVALID_HANDLE_VALUE) break;

        if (GetLastError() != ERROR_PIPE_BUSY) {
            printf("Не удалось найти компьютер '%s' или сервер не запущен.\n", serverName);
            return;
        }
        WaitNamedPipeA(pipePath, 5000);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    char message[BUF_SIZE];
    char sendBuffer[BUF_SIZE];
    DWORD bytesWritten, bytesRead;

    printf("Подключено к %s! Введите 'выход' для отключения.\n", serverName);
    while (1) {
        printf("Вы: ");
        fgets(message, BUF_SIZE, stdin);
        message[strcspn(message, "\n")] = 0; // Удаляем newline

        if (strcmp(message, "выход") == 0) {
            WriteFile(hPipe, message, strlen(message), &bytesWritten, NULL);
            break;
        }

        sprintf(sendBuffer, "MSG:%s", message);
        // SaveToHistory("You", message); // Раскомментируйте, если добавили функцию сохранения

        WriteFile(hPipe, sendBuffer, strlen(sendBuffer), &bytesWritten, NULL);

        if (ReadFile(hPipe, sendBuffer, BUF_SIZE - 1, &bytesRead, NULL)) {
            sendBuffer[bytesRead] = '\0';
            printf("[%s]: %s\n", serverName, sendBuffer);
        }
    }
    CloseHandle(hPipe);
}

// ... (Client_BroadcastMsg остается без изменений) ...

int main() {
    SetConsoleOutputCP(1251); // UTF-8 для консоли
    SetConsoleCP(1251);

    // Узнаем имя текущего компьютера, чтобы показать его пользователю
    char myComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD nameSize = sizeof(myComputerName);
    GetComputerNameA(myComputerName, &nameSize);

    printf("=========================================\n");
    printf("Ваше имя компьютера в сети: %s\n", myComputerName);
    printf("Сообщите его собеседнику для подключения.\n");
    printf("=========================================\n");

    // Запускаем серверные потоки в фоне
    // CreateThread(NULL, 0, PipeServerThread, NULL, 0, NULL);
    // CreateThread(NULL, 0, MailslotServerThread, NULL, 0, NULL);

    int choice;
    char targetName[128]; // Буфер для имени компьютера

    while (1) {
        printf("\n--- МЕНЮ ---\n");
        printf("1. Подключиться к ПК по имени (Чат)\n");
        printf("2. Отправить сообщение ВСЕМ в сети (Broadcast)\n");
        printf("3. Просмотреть историю переписки\n");
        printf("0. Выйти\n");
        printf("Ваш выбор: ");

        if (scanf("%d", &choice) != 1) break;
        while (getchar() != '\n'); // Очистка буфера от '\n'

        switch (choice) {
        case 1:
            printf("Введите ИМЯ КОМПЬЮТЕРА (например DESKTOP-123 или '.' для локального): ");
            fgets(targetName, sizeof(targetName), stdin);
            targetName[strcspn(targetName, "\n")] = 0;

            if (strlen(targetName) > 0) {
                Client_ChatMode(targetName);
            }
            break;
        case 2:
            // Client_BroadcastMsg();
            break;
        case 3:
            // Чтение истории...
            break;
        case 0:
            exit(0);
        default:
            printf("Неверный выбор.\n");
        }
    }
    return 0;
}