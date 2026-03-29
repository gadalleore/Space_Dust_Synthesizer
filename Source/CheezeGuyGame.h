#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// Cheeze Guy - Easter egg mini-game
// A tribute to the first game ever built using AI.
// Click the master knob 7 times in 5 seconds to play!
//==============================================================================

class CheezeGuyGameComponent : public juce::Component, public juce::Timer
{
public:
    CheezeGuyGameComponent()
    {
        setSize(600, 500);
        setWantsKeyboardFocus(true);
        resetGame();
    }

    ~CheezeGuyGameComponent() override
    {
        stopTimer();
    }

    void resetGame()
    {
        stopTimer();
        playerX = 300.0f;
        playerY = 250.0f;
        playerDir = Right;
        playerSize = 20.0f;
        score = 0;
        timeRemaining = 60.0f;
        maxTime = 60.0f;
        gameOver = false;
        gameStarted = false;
        spawnCheezeball();
        repaint();
    }

    void timerCallback() override
    {
        if (gameOver || !gameStarted) return;

        // Move player
        switch (playerDir)
        {
            case Up:    playerY -= playerSpeed; break;
            case Down:  playerY += playerSpeed; break;
            case Left:  playerX -= playerSpeed; break;
            case Right: playerX += playerSpeed; break;
        }

        // Wall collision - check all edges against player bounds
        float halfLen = playerSize * 0.6f;
        float halfWid = playerSize * 0.35f;
        float extentX = (playerDir == Left || playerDir == Right) ? halfLen : halfWid;
        float extentY = (playerDir == Up || playerDir == Down) ? halfLen : halfWid;

        if (playerX - extentX < 0.0f || playerX + extentX > (float)getWidth() ||
            playerY - extentY < 0.0f || playerY + extentY > (float)getHeight())
        {
            gameOver = true;
            stopTimer();
            repaint();
            return;
        }

        // Cheezeball collision
        float dx = playerX - cheezeX;
        float dy = playerY - cheezeY;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < playerSize * 0.5f + cheezeRadius)
        {
            score++;
            playerSize += 1.5f;
            maxTime = juce::jmax(5.0f, maxTime - 0.5f);
            timeRemaining = maxTime;
            spawnCheezeball();
        }

        // Timer countdown (~60fps, 16ms per tick)
        timeRemaining -= 16.0f / 1000.0f;
        if (timeRemaining <= 0.0f)
        {
            timeRemaining = 0.0f;
            gameOver = true;
            stopTimer();
        }

        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        // Starry background
        g.fillAll(juce::Colour(0xff0a0a2f));
        {
            juce::Random starRng(42);
            for (int i = 0; i < 60; i++)
            {
                float alpha = 0.15f + starRng.nextFloat() * 0.25f;
                g.setColour(juce::Colours::white.withAlpha(alpha));
                float sx = starRng.nextFloat() * (float)getWidth();
                float sy = starRng.nextFloat() * (float)getHeight();
                float sr = 0.5f + starRng.nextFloat() * 1.5f;
                g.fillEllipse(sx, sy, sr, sr);
            }
        }

        // Border
        g.setColour(juce::Colour(0xff00d4ff).withAlpha(0.15f));
        g.drawRect(getLocalBounds(), 2);

        if (!gameStarted && !gameOver)
        {
            paintTitleScreen(g);
            return;
        }

        // Cheezeball
        drawCheezeball(g, cheezeX, cheezeY, cheezeRadius);

        // Player
        drawPlayer(g);

        // HUD - score
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(16.0f).boldened());
        g.drawText("Cheeze Balls: " + juce::String(score),
                   10, 8, 200, 25, juce::Justification::left);

        // HUD - timer bar
        paintTimerBar(g);

        // Game over overlay
        if (gameOver)
            paintGameOver(g);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (gameOver) return false;

        bool isArrow = (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey ||
                        key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey);

        if (!gameStarted && isArrow)
        {
            if (key == juce::KeyPress::upKey)    playerDir = Up;
            if (key == juce::KeyPress::downKey)  playerDir = Down;
            if (key == juce::KeyPress::leftKey)  playerDir = Left;
            if (key == juce::KeyPress::rightKey) playerDir = Right;
            gameStarted = true;
            startTimer(16);
            return true;
        }

        // Prevent 180-degree turns (snake rules)
        if (key == juce::KeyPress::upKey    && playerDir != Down)  { playerDir = Up;    return true; }
        if (key == juce::KeyPress::downKey  && playerDir != Up)    { playerDir = Down;  return true; }
        if (key == juce::KeyPress::leftKey  && playerDir != Right) { playerDir = Left;  return true; }
        if (key == juce::KeyPress::rightKey && playerDir != Left)  { playerDir = Right; return true; }

        return false;
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (gameOver)
        {
            resetGame();
            grabKeyboardFocus();
            return;
        }
        grabKeyboardFocus();
    }

private:
    enum Direction { Up, Down, Left, Right };

    float playerX = 300.0f, playerY = 250.0f;
    float playerSize = 20.0f;
    static constexpr float playerSpeed = 3.0f;
    Direction playerDir = Right;

    float cheezeX = 0.0f, cheezeY = 0.0f;
    static constexpr float cheezeRadius = 8.0f;

    int score = 0;
    float timeRemaining = 60.0f;
    float maxTime = 60.0f;
    bool gameOver = false;
    bool gameStarted = false;

    juce::Random rng;

    //==============================================================================
    void spawnCheezeball()
    {
        int w = juce::jmax(100, getWidth());
        int h = juce::jmax(100, getHeight());
        float margin = 40.0f;

        for (int i = 0; i < 200; i++)
        {
            cheezeX = margin + rng.nextFloat() * ((float)w - margin * 2.0f);
            cheezeY = margin + rng.nextFloat() * ((float)h - margin * 2.0f);

            float dx = playerX - cheezeX;
            float dy = playerY - cheezeY;
            if (std::sqrt(dx * dx + dy * dy) > playerSize * 3.0f)
                return;
        }
    }

    //==============================================================================
    void drawCheezeball(juce::Graphics& g, float cx, float cy, float r) const
    {
        // Main ball
        g.setColour(juce::Colour(0xffffd700));
        g.fillEllipse(cx - r, cy - r, r * 2.0f, r * 2.0f);
        // Cheese holes
        g.setColour(juce::Colour(0xffcc9900));
        g.fillEllipse(cx - 3.0f, cy - 2.0f, 4.0f, 3.0f);
        g.fillEllipse(cx + 2.0f, cy + 2.0f, 3.0f, 2.0f);
    }

    void drawPlayer(juce::Graphics& g) const
    {
        float halfLen = playerSize * 0.6f;
        float halfWid = playerSize * 0.35f;

        float tipX = playerX, tipY = playerY;
        float b1X = playerX, b1Y = playerY, b2X = playerX, b2Y = playerY;

        switch (playerDir)
        {
            case Right: tipX += halfLen; b1X -= halfLen; b1Y -= halfWid; b2X -= halfLen; b2Y += halfWid; break;
            case Left:  tipX -= halfLen; b1X += halfLen; b1Y -= halfWid; b2X += halfLen; b2Y += halfWid; break;
            case Up:    tipY -= halfLen; b1X -= halfWid; b1Y += halfLen; b2X += halfWid; b2Y += halfLen; break;
            case Down:  tipY += halfLen; b1X -= halfWid; b1Y -= halfLen; b2X += halfWid; b2Y -= halfLen; break;
        }

        juce::Path tri;
        tri.addTriangle(tipX, tipY, b1X, b1Y, b2X, b2Y);

        // Glow
        g.setColour(juce::Colour(0xff00d4ff).withAlpha(0.15f));
        g.strokePath(tri, juce::PathStrokeType(5.0f));

        // Body
        g.setColour(juce::Colour(0xff00d4ff));
        g.fillPath(tri);

        // Eye (small dot near the tip)
        float eyeOff = halfLen * 0.35f;
        float eyeX = playerX, eyeY = playerY;
        switch (playerDir)
        {
            case Right: eyeX += eyeOff; break;
            case Left:  eyeX -= eyeOff; break;
            case Up:    eyeY -= eyeOff; break;
            case Down:  eyeY += eyeOff; break;
        }
        g.setColour(juce::Colours::white);
        g.fillEllipse(eyeX - 2.0f, eyeY - 2.0f, 4.0f, 4.0f);
        g.setColour(juce::Colour(0xff0a0a2f));
        g.fillEllipse(eyeX - 1.0f, eyeY - 1.0f, 2.0f, 2.0f);
    }

    //==============================================================================
    void paintTitleScreen(juce::Graphics& g) const
    {
        g.setColour(juce::Colour(0xffffd700));
        g.setFont(juce::Font(44.0f).boldened());
        g.drawText("CHEEZE GUY", 0, 120, getWidth(), 50, juce::Justification::centred);

        g.setColour(juce::Colour(0xffa0d8ff));
        g.setFont(juce::Font(18.0f));
        g.drawText("Press any arrow key to start!", 0, 220, getWidth(), 30, juce::Justification::centred);

        g.setColour(juce::Colour(0xff7090b0));
        g.setFont(juce::Font(14.0f));
        g.drawText("Eat the cheeze balls before time runs out!", 0, 280, getWidth(), 25, juce::Justification::centred);
        g.drawText("Don't hit the walls!", 0, 305, getWidth(), 25, juce::Justification::centred);

        // Decorative cheezeball
        drawCheezeball(g, 300.0f, 375.0f, 15.0f);
    }

    void paintTimerBar(juce::Graphics& g) const
    {
        float frac = juce::jlimit(0.0f, 1.0f, timeRemaining / maxTime);
        auto barColour = frac > 0.3f ? juce::Colour(0xff00d4ff) : juce::Colour(0xffff4444);
        int barW = 200, barH = 18;
        int barX = getWidth() - barW - 10, barY = 10;

        g.setColour(barColour.withAlpha(0.2f));
        g.fillRect(barX, barY, barW, barH);
        g.setColour(barColour);
        g.fillRect(barX, barY, (int)((float)barW * frac), barH);
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(13.0f));
        g.drawText(juce::String(timeRemaining, 1) + "s",
                   barX, barY, barW, barH, juce::Justification::centred);
    }

    void paintGameOver(juce::Graphics& g) const
    {
        g.setColour(juce::Colour(0xbb000000));
        g.fillRect(getLocalBounds());

        g.setColour(juce::Colour(0xffff4444));
        g.setFont(juce::Font(52.0f).boldened());
        g.drawText("GAME OVER", 0, 140, getWidth(), 60, juce::Justification::centred);

        g.setColour(juce::Colour(0xffffd700));
        g.setFont(juce::Font(26.0f).boldened());
        g.drawText("Cheeze Balls Eaten: " + juce::String(score),
                   0, 230, getWidth(), 35, juce::Justification::centred);

        g.setColour(juce::Colour(0xffa0d8ff));
        g.setFont(juce::Font(16.0f));
        g.drawText("Click to play again", 0, 310, getWidth(), 25, juce::Justification::centred);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CheezeGuyGameComponent)
};

//==============================================================================
class CheezeGuyWindow : public juce::DocumentWindow
{
public:
    CheezeGuyWindow()
        : DocumentWindow("Cheeze Guy", juce::Colour(0xff0a0a2f),
                          DocumentWindow::closeButton)
    {
        auto* game = new CheezeGuyGameComponent();
        setContentOwned(game, true);
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        centreWithSize(game->getWidth(), game->getHeight());
        setVisible(true);
        toFront(true);
        game->grabKeyboardFocus();
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }

    void showGame()
    {
        setVisible(true);
        toFront(true);
        if (auto* game = dynamic_cast<CheezeGuyGameComponent*>(getContentComponent()))
        {
            game->resetGame();
            game->grabKeyboardFocus();
        }
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CheezeGuyWindow)
};
