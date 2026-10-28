#include <string>
#include "raylib.h"

// Funzione del menu principale con splash screen e layout pulito a sinistra
std::string RunMainMenu(Font customFont)
{
    float screenW = (float)GetScreenWidth();
    float screenH = (float)GetScreenHeight();

    // 1. SPLASH SCREEN CON ST_Logo.png (leggermente più piccolo rispetto a prima)
    Texture2D splashLogo = LoadTexture("assets/MainMenu/ST_Logo.png");
    double splashStartTime = GetTime();
    bool splashActive = true;

    while (!WindowShouldClose() && splashActive)
    {
        double elapsed = GetTime() - splashStartTime;
        
        float alpha = 0.0f;
        if (elapsed < 0.8) {
            alpha = (float)(elapsed / 0.8);
        } else if (elapsed < 1.8) {
            alpha = 1.0f;
        } else if (elapsed < 2.5) {
            alpha = (float)(1.0f - (elapsed - 1.8) / 0.7);
        } else {
            splashActive = false;
        }

        if (IsKeyPressed(KEY_ESCAPE) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            splashActive = false;
        }

        BeginDrawing();
        ClearBackground(BLACK);

        if (splashLogo.id > 0 && alpha > 0.0f)
        {
            float scale = 1.0f; // Leggermente ridotto
            int w = (int)(splashLogo.width * scale);
            int h = (int)(splashLogo.height * scale);
            Rectangle dest = { (screenW - w) / 2.0f, (screenH - h) / 2.0f, (float)w, (float)h };
            Rectangle source = { 0, 0, (float)splashLogo.width, (float)splashLogo.height };
            DrawTexturePro(splashLogo, source, dest, {0, 0}, 0.0f, Fade(WHITE, alpha));
        }

        EndDrawing();
    }

    if (splashLogo.id > 0) UnloadTexture(splashLogo);

    if (WindowShouldClose())
    {
        return "";
    }

    // 2. MENU PRINCIPALE
    Texture2D logoTexture = LoadTexture("assets/MainMenu/TT_Logo.png");

    std::string ipText = "127.0.0.1";
    
    if (FileExists("server_ip.txt"))
    {
        char* saved = LoadFileText("server_ip.txt");
        if (saved)
        {
            std::string t = saved;
            UnloadFileText(saved);
            while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
            if (!t.empty()) ipText = t;
        }
    }

    std::string chosenIp = "127.0.0.1";
    bool startGame = false;

    while (!WindowShouldClose() && !startGame)
    {
        screenW = (float)GetScreenWidth();
        screenH = (float)GetScreenHeight();

        // Coordinate bloccate in basso a sinistra (stile immagine di riferimento)
        float leftMargin = 50.0f;
        float startY = screenH - 320.0f;

        Rectangle boxIp     = { leftMargin, startY, 320, 40 };
        Rectangle btnOnline = { leftMargin, startY + 52, 320, 45 };
        Rectangle btnLocal  = { leftMargin, startY + 107, 320, 45 };

        Vector2 mousePos = GetMousePosition();

        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        int ch;
        while ((ch = GetCharPressed()) > 0)
            if (!ctrl && ch >= 33 && ch < 127 && ipText.size() < 60) ipText.push_back((char)ch);
        
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !ipText.empty()) 
            ipText.pop_back();

        if (ctrl && IsKeyPressed(KEY_V))
        {
            const char* clip = GetClipboardText();
            if (clip)
                for (const char* p = clip; *p && ipText.size() < 60; ++p)
                    if (*p >= 33 && *p < 127) ipText.push_back(*p);
        }

        if (IsKeyPressed(KEY_ENTER) && !ipText.empty())
        {
            chosenIp = ipText;
            startGame = true;
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            if (CheckCollisionPointRec(mousePos, btnOnline))
            {
                if (!ipText.empty())
                {
                    chosenIp = ipText;
                    startGame = true;
                }
            }
            else if (CheckCollisionPointRec(mousePos, btnLocal))
            {
                chosenIp = "127.0.0.1";
                startGame = true;
            }
        }

        BeginDrawing();
        ClearBackground({ 76, 153, 114, 255 }); // Sfondo verde simile all'immagine

        // Logo del gioco più grande in alto a sinistra, staccato dai bordi[cite: 1]
        if (logoTexture.id > 0)
        {
            float scale = 0.65f; // Abbastanza più grande di prima
            int w = (int)(logoTexture.width * scale);
            int h = (int)(logoTexture.height * scale);
            Rectangle dest = { 40.0f, 40.0f, (float)w, (float)h };
            Rectangle source = { 0, 0, (float)logoTexture.width, (float)logoTexture.height };
            DrawTexturePro(logoTexture, source, dest, {0, 0}, 0.0f, WHITE);
        }

        // Casella di testo IP
        DrawRectangleRec(boxIp, { 30, 40, 50, 255 });
        DrawRectangleLinesEx(boxIp, 2, WHITE);
        {
            std::string shown = ipText + (((int)(GetTime() * 2.0) % 2 == 0) ? "_" : "");
            DrawTextEx(customFont, shown.c_str(), {boxIp.x + 12, boxIp.y + 10}, 16.0f, 1.0f, WHITE);
        }

        // Pulsante Online
        bool hoverOnline = CheckCollisionPointRec(mousePos, btnOnline);
        DrawRectangleRec(btnOnline, hoverOnline ? (Color){ 90, 160, 120, 255 } : (Color){ 60, 120, 90, 255 });
        DrawRectangleLinesEx(btnOnline, 2, WHITE);
        DrawTextEx(customFont, "Multiplayer (IP sopra)", {btnOnline.x + 15, btnOnline.y + 13}, 16.0f, 1.0f, WHITE);

        // Pulsante Localhost
        bool hoverLocal = CheckCollisionPointRec(mousePos, btnLocal);
        DrawRectangleRec(btnLocal, hoverLocal ? (Color){ 90, 160, 120, 255 } : (Color){ 60, 120, 90, 255 });
        DrawRectangleLinesEx(btnLocal, 2, WHITE);
        DrawTextEx(customFont, "Localhost (127.0.0.1)", {btnLocal.x + 15, btnLocal.y + 13}, 16.0f, 1.0f, WHITE);

        EndDrawing();
    }

    if (logoTexture.id > 0) UnloadTexture(logoTexture);

    if (WindowShouldClose())
    {
        return "";
    }

    SaveFileText("server_ip.txt", (char*)(chosenIp == "127.0.0.1" ? ipText : chosenIp).c_str());
    return chosenIp;
}