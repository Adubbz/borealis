/*
    Borealis, a Nintendo Switch UI Library
    Copyright (C) 2019-2020  natinusala
    Copyright (C) 2019  p-sam
    Copyright (C) 2020  WerWolv

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

#include <nanovg.h>
#include <nanovg_dk.h>
#include <nanovg/framework/CCmdMemRing.h>

#include <algorithm>
#include <borealis.hpp>
#include <string>

#include <switch.h>

#include <chrono>
#include <set>
#include <optional>
#include <thread>

// Constants used for scaling as well as
// creating a window of the right size on PC
constexpr uint32_t WINDOW_WIDTH  = 1280;
constexpr uint32_t WINDOW_HEIGHT = 720;

#define DEFAULT_FPS 60
#define BUTTON_REPEAT_DELAY 15
#define BUTTON_REPEAT_CADENCY 5
namespace brls
{

namespace {
    class DkRenderer final {
        static constexpr unsigned NumFramebuffers = 2;
        static constexpr unsigned StaticCmdSize = 0x1000;
        static constexpr unsigned DynCmdSize = 0x1000;

        dk::UniqueDevice device;
        dk::UniqueQueue queue;

        std::optional<CMemPool> pool_images;
        std::optional<CMemPool> pool_code;
        std::optional<CMemPool> pool_data;

        dk::UniqueCmdBuf cmdbuf;
        dk::UniqueCmdBuf dynCmdBuf;
        CCmdMemRing<NumFramebuffers> dynCmdMem;

        CMemPool::Handle depthBuffer_mem;
        CMemPool::Handle framebuffers_mem[NumFramebuffers];

        dk::Image depthBuffer;
        dk::Image framebuffers[NumFramebuffers];
        DkCmdList framebuffer_cmdlists[NumFramebuffers];
        dk::UniqueSwapchain swapchain;

        DkCmdList render_cmdlist;

        std::optional<nvg::DkRenderer> renderer;
        NVGcontext* vg;

        int slot = -1;

    public:
        DkRenderer()
        {
            // Create the deko3d device
            device = dk::DeviceMaker{}.create();

            // Create the main queue
            queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();

            // Create the memory pools
            pool_images.emplace(device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 16*1024*1024);
            pool_code.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, 128*1024);
            pool_data.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 1*1024*1024);

            // Create the static command buffer and feed it freshly allocated memory
            cmdbuf = dk::CmdBufMaker{device}.create();
            CMemPool::Handle cmdmem = pool_data->allocate(StaticCmdSize);
            cmdbuf.addMemory(cmdmem.getMemBlock(), cmdmem.getOffset(), cmdmem.getSize());

            dynCmdBuf = dk::CmdBufMaker{device}.create();
            dynCmdMem.allocate(*pool_data, DynCmdSize);

            // Create the framebuffer resources
            createFramebufferResources();

            this->renderer.emplace(WINDOW_WIDTH, WINDOW_HEIGHT, this->device, this->queue, *this->pool_images, *this->pool_code, *this->pool_data);
            this->vg = nvgCreateDk(&*this->renderer, NVG_ANTIALIAS | NVG_STENCIL_STROKES);
        }

        ~DkRenderer()
        {
            // Cleanup vg. This needs to be done first as it relies on the renderer.
            nvgDeleteDk(vg);

            // Destroy the renderer
            this->renderer.reset();

            // Destroy the framebuffer resources
            destroyFramebufferResources();
        }

        void createFramebufferResources()
        {
        // Create layout for the depth buffer
            dk::ImageLayout layout_depthbuffer;
            dk::ImageLayoutMaker{device}
                .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
                .setFormat(DkImageFormat_S8)
                .setDimensions(WINDOW_WIDTH, WINDOW_HEIGHT)
                .initialize(layout_depthbuffer);

            // Create the depth buffer
            depthBuffer_mem = pool_images->allocate(layout_depthbuffer.getSize(), layout_depthbuffer.getAlignment());
            depthBuffer.initialize(layout_depthbuffer, depthBuffer_mem.getMemBlock(), depthBuffer_mem.getOffset());

            // Create layout for the framebuffers
            dk::ImageLayout layout_framebuffer;
            dk::ImageLayoutMaker{device}
                .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
                .setFormat(DkImageFormat_RGBA8_Unorm)
                .setDimensions(WINDOW_WIDTH, WINDOW_HEIGHT)
                .initialize(layout_framebuffer);

            // Create the framebuffers
            std::array<DkImage const*, NumFramebuffers> fb_array;
            uint64_t fb_size  = layout_framebuffer.getSize();
            uint32_t fb_align = layout_framebuffer.getAlignment();
            for (unsigned i = 0; i < NumFramebuffers; i ++)
            {
                // Allocate a framebuffer
                framebuffers_mem[i] = pool_images->allocate(fb_size, fb_align);
                framebuffers[i].initialize(layout_framebuffer, framebuffers_mem[i].getMemBlock(), framebuffers_mem[i].getOffset());

                // Generate a command list that binds it
                dk::ImageView colorTarget{ framebuffers[i] }, depthTarget{ depthBuffer };
                cmdbuf.bindRenderTargets(&colorTarget, &depthTarget);
                framebuffer_cmdlists[i] = cmdbuf.finishList();

                // Fill in the array for use later by the swapchain creation code
                fb_array[i] = &framebuffers[i];
            }

            // Create the swapchain using the framebuffers
            swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), fb_array}.create();

            // Generate the main rendering cmdlist
            recordStaticCommands();
        }

        void destroyFramebufferResources()
        {
            // Return early if we have nothing to destroy
            if (!swapchain) return;

            // Make sure the queue is idle before destroying anything
            queue.waitIdle();

            // Clear the static cmdbuf, destroying the static cmdlists in the process
            cmdbuf.clear();

            // Destroy the swapchain
            swapchain.destroy();

            // Destroy the framebuffers
            for (unsigned i = 0; i < NumFramebuffers; i ++)
            {
                framebuffers_mem[i].destroy();
            }

            // Destroy the depth buffer
            depthBuffer_mem.destroy();
        }

        void recordStaticCommands()
        {
            // Initialize state structs with deko3d defaults
            dk::RasterizerState rasterizerState;
            dk::ColorState colorState;
            dk::ColorWriteState colorWriteState;
            dk::BlendState blendState;

            // Configure the viewport and scissor
            cmdbuf.setViewports(0, { { 0.0f, 0.0f, WINDOW_WIDTH, WINDOW_HEIGHT, 0.0f, 1.0f } });
            cmdbuf.setScissors(0, { { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT } });

            // Clear the color and depth buffers
            cmdbuf.clearDepthStencil(true, 1.0f, 0xFF, 0);

            // Bind required state
            cmdbuf.bindRasterizerState(rasterizerState);
            cmdbuf.bindColorState(colorState);
            cmdbuf.bindColorWriteState(colorWriteState);

            render_cmdlist = cmdbuf.finishList();
        }

        void clearWithColor(float r, float g, float b)
        {
            dynCmdMem.begin(dynCmdBuf);
            dynCmdBuf.clearColor(0, DkColorMask_RGBA, r, g, b, 1.0f);
            queue.submitCommands(dynCmdMem.end(dynCmdBuf));
        }

        void beginRender()
        {
            // Acquire a framebuffer from the swapchain (and wait for it to be available)
            this->slot = queue.acquireImage(swapchain);

            // Run the command list that attaches said framebuffer to the queue
            queue.submitCommands(framebuffer_cmdlists[slot]);

            // Run the main rendering command list
            queue.submitCommands(render_cmdlist);

        }

        void endRender()
        {

            // Now that we are done rendering, present it to the screen
            queue.presentImage(swapchain, slot);
        }

        NVGcontext *getNvgContext()
        {
            return this->vg;
        }
    };

    std::optional<DkRenderer> dk;
}

bool Application::init(std::string title)
{
    return Application::init(title, Style::horizon(), Theme::horizon());
}

bool Application::init(std::string title, Style style, Theme theme)
{
    // Init rng
    std::srand(std::time(nullptr));

    // Init managers
    Application::taskManager         = new TaskManager();
    Application::notificationManager = new NotificationManager();

    // Init static variables
    Application::currentStyle = style;
    Application::currentFocus = nullptr;
    Application::title        = title;

    // Init theme to defaults
    Application::setTheme(theme);

    Application::windowScale = 1.0f;

    float contentHeight = ((float)WINDOW_HEIGHT / (Application::windowScale * (float)WINDOW_HEIGHT)) * (float)WINDOW_HEIGHT;

    dk.emplace();
    Application::vg = dk->getNvgContext();

    Application::contentWidth  = WINDOW_WIDTH;
    Application::contentHeight = (unsigned)roundf(contentHeight);

    Application::resizeNotificationManager();

    // Load fonts
#ifdef __SWITCH__
    {
        PlFontData font;

        // Standard font
        Result rc = plGetSharedFontByType(&font, PlSharedFontType_Standard);
        if (R_SUCCEEDED(rc))
        {
            Logger::info("Using Switch shared font");
            Application::fontStash.regular = Application::loadFontFromMemory("regular", font.address, font.size, false);
        }

        // Korean font
        rc = plGetSharedFontByType(&font, PlSharedFontType_KO);
        if (R_SUCCEEDED(rc))
        {
            Logger::info("Adding Switch shared Korean font");
            Application::fontStash.korean = Application::loadFontFromMemory("korean", font.address, font.size, false);
            nvgAddFallbackFontId(Application::vg, Application::fontStash.regular, Application::fontStash.korean);
        }

        // Extented font
        rc = plGetSharedFontByType(&font, PlSharedFontType_NintendoExt);
        if (R_SUCCEEDED(rc))
        {
            Logger::info("Using Switch shared symbols font");
            Application::fontStash.sharedSymbols = Application::loadFontFromMemory("symbols", font.address, font.size, false);
        }
    }
#else
    // Use illegal font if available
    if (access(BOREALIS_ASSET("Illegal-Font.ttf"), F_OK) != -1)
        Application::fontStash.regular = Application::loadFont("regular", BOREALIS_ASSET("Illegal-Font.ttf"));
    else
        Application::fontStash.regular = Application::loadFont("regular", BOREALIS_ASSET("inter/Inter-Switch.ttf"));

    if (access(BOREALIS_ASSET("Wingdings.ttf"), F_OK) != -1)
        Application::fontStash.sharedSymbols = Application::loadFont("sharedSymbols", BOREALIS_ASSET("Wingdings.ttf"));
#endif

    // Material font
    if (access(BOREALIS_ASSET("material/MaterialIcons-Regular.ttf"), F_OK) != -1)
        Application::fontStash.material = Application::loadFont("material", BOREALIS_ASSET("material/MaterialIcons-Regular.ttf"));

    // Set symbols font as fallback
    if (Application::fontStash.sharedSymbols)
    {
        Logger::info("Using shared symbols font");
        nvgAddFallbackFontId(Application::vg, Application::fontStash.regular, Application::fontStash.sharedSymbols);
    }
    else
    {
        Logger::error("Shared symbols font not found");
    }

    // Set Material as fallback
    if (Application::fontStash.material)
    {
        Logger::info("Using Material font");
        nvgAddFallbackFontId(Application::vg, Application::fontStash.regular, Application::fontStash.material);
    }
    else
    {
        Logger::error("Material font not found");
    }

    // Load theme
#ifdef __SWITCH__
    ColorSetId nxTheme;
    setsysGetColorSetId(&nxTheme);

    if (nxTheme == ColorSetId_Dark)
        Application::currentThemeVariant = ThemeVariant_DARK;
    else
        Application::currentThemeVariant = ThemeVariant_LIGHT;
#else
    char* themeEnv = getenv("BOREALIS_THEME");
    if (themeEnv != nullptr && !strcasecmp(themeEnv, "DARK"))
        Application::currentThemeVariant = ThemeVariant_DARK;
    else
        Application::currentThemeVariant = ThemeVariant_LIGHT;
#endif

    Application::windowWidth  = WINDOW_WIDTH;
    Application::windowHeight = WINDOW_HEIGHT;

    // Init animations engine
    menu_animation_init();

    // Default FPS cap
    Application::setMaximumFPS(DEFAULT_FPS);

    return true;
}

bool Application::mainLoop()
{
    // Frame start
    retro_time_t frameStart = 0;
    if (Application::frameTime > 0.0f)
        frameStart = cpu_features_get_time_usec();

    if (!appletMainLoop())
    {
        Application::exit();
        return false;
    }

    // Trigger gamepad events
    // TODO: Translate axis events to dpad events here

    bool anyButtonPressed               = false;
    bool repeating                      = false;
    static retro_time_t buttonPressTime = 0;
    static int repeatingButtonTimer     = 0;

    hidScanInput();
    u64 kDown = hidKeysHeld(CONTROLLER_P1_AUTO);

    for (u64 i = 0; i < MaxButtons; i++)
    {
        u64 button = (1 << i);
        u8 down = (kDown & button) != 0;

        if (down)
        {
            anyButtonPressed = true;
            repeating        = (repeatingButtonTimer > BUTTON_REPEAT_DELAY && repeatingButtonTimer % BUTTON_REPEAT_CADENCY == 0);

            if (Application::buttons[i] != down || repeating)
                Application::onGamepadButtonPressed(button, repeating);
        }

        if (Application::buttons[i] != down)
            buttonPressTime = repeatingButtonTimer = 0;

        Application::buttons[i] = down;
    }

    if (anyButtonPressed && cpu_features_get_time_usec() - buttonPressTime > 1000)
    {
        buttonPressTime = cpu_features_get_time_usec();
        repeatingButtonTimer++; // Increased once every ~1ms
    }

    if (kDown & KEY_PLUS) {
        Application::exit();
        return false;
    }

    // Animations
    menu_animation_update();

    // Tasks
    Application::taskManager->frame();

    // Render
    Application::frame();

    // Sleep if necessary
    if (Application::frameTime > 0.0f)
    {
        retro_time_t currentFrameTime = cpu_features_get_time_usec() - frameStart;
        retro_time_t frameTime        = (retro_time_t)(Application::frameTime * 1000);

        if (frameTime > currentFrameTime)
        {
            retro_time_t toSleep = frameTime - currentFrameTime;
            std::this_thread::sleep_for(std::chrono::microseconds(toSleep));
        }
    }

    return true;
}

void Application::quit()
{

}

void Application::navigate(FocusDirection direction)
{
    View* currentFocus = Application::currentFocus;

    // Do nothing if there is no current focus or if it doesn't have a parent
    // (in which case there is nothing to traverse)
    if (!currentFocus || !currentFocus->hasParent())
        return;

    // Get next view to focus by traversing the views tree upwards
    View* nextFocus = currentFocus->getParent()->getNextFocus(direction, currentFocus->getParentUserData());

    while (!nextFocus) // stop when we find a view to focus
    {
        if (!currentFocus->hasParent() || !currentFocus->getParent()->hasParent()) // stop when we reach the root of the tree
            break;

        currentFocus = currentFocus->getParent();
        nextFocus    = currentFocus->getParent()->getNextFocus(direction, currentFocus->getParentUserData());
    }

    // No view to focus at the end of the traversal: wiggle and return
    if (!nextFocus)
    {
        Application::currentFocus->shakeHighlight(direction);
        return;
    }

    // Otherwise give it focus
    Application::giveFocus(nextFocus);
}

void Application::onGamepadButtonPressed(u64 button, bool repeating)
{
    if (Application::blockInputsTokens != 0)
        return;

    if (repeating && Application::repetitionOldFocus == Application::currentFocus)
        return;

    Application::repetitionOldFocus = Application::currentFocus;

    // Actions
    if (Application::handleAction(button))
        return;

    // Navigation
    // Only navigate if the button hasn't been consumed by an action
    // (allows overriding DPAD buttons using actions)
    if (button & KEY_DDOWN) {
        Application::navigate(FocusDirection::DOWN);
    } else if (button & KEY_DUP) {
        Application::navigate(FocusDirection::UP);
    } else if (button & KEY_DLEFT) {
        Application::navigate(FocusDirection::LEFT);
    } else if (button & KEY_DRIGHT) {
        Application::navigate(FocusDirection::RIGHT);
    }
}

View* Application::getCurrentFocus()
{
    return Application::currentFocus;
}

bool Application::handleAction(u64 button)
{
    View* hintParent = Application::currentFocus;
    std::set<Key> consumedKeys;

    while (hintParent != nullptr)
    {
        for (auto& action : hintParent->getActions())
        {
            if (action.key != static_cast<Key>(button))
                continue;

            if (consumedKeys.find(action.key) != consumedKeys.end())
                continue;

            if (action.available)
                if (action.actionListener())
                    consumedKeys.insert(action.key);
        }

        hintParent = hintParent->getParent();
    }

    return !consumedKeys.empty();
}

void Application::frame()
{
    // Frame context
    FrameContext frameContext = FrameContext();

    frameContext.pixelRatio = (float)Application::windowWidth / (float)Application::windowHeight;
    frameContext.vg         = Application::vg;
    frameContext.fontStash  = &Application::fontStash;
    frameContext.theme      = Application::getThemeValues();

    dk->beginRender();

    dk->clearWithColor(frameContext.theme->backgroundColor[0], frameContext.theme->backgroundColor[1], frameContext.theme->backgroundColor[2]);

    nvgBeginFrame(Application::vg, Application::windowWidth, Application::windowHeight, frameContext.pixelRatio);
    nvgScale(Application::vg, Application::windowScale, Application::windowScale);

    std::vector<View*> viewsToDraw;

    // Draw all views in the stack
    // until we find one that's not translucent
    for (size_t i = 0; i < Application::viewStack.size(); i++)
    {
        View* view = Application::viewStack[Application::viewStack.size() - 1 - i];
        viewsToDraw.push_back(view);

        if (!view->isTranslucent())
            break;
    }

    for (size_t i = 0; i < viewsToDraw.size(); i++)
    {
        View* view = viewsToDraw[viewsToDraw.size() - 1 - i];
        view->frame(&frameContext);
    }

    // Framerate counter
    if (Application::framerateCounter)
        Application::framerateCounter->frame(&frameContext);

    // Notifications
    Application::notificationManager->frame(&frameContext);

    // End frame
    nvgResetTransform(Application::vg); // scale
    nvgEndFrame(Application::vg);

    dk->endRender();
}

void Application::exit()
{
    Application::clear();

    menu_animation_free();

    if (Application::framerateCounter)
        delete Application::framerateCounter;

    delete Application::taskManager;
    delete Application::notificationManager;
}

void Application::setDisplayFramerate(bool enabled)
{
    if (!Application::framerateCounter && enabled)
    {
        Logger::info("Enabling framerate counter");
        Application::framerateCounter = new FramerateCounter();
        Application::resizeFramerateCounter();
    }
    else if (Application::framerateCounter && !enabled)
    {
        Logger::info("Disabling framerate counter");
        delete Application::framerateCounter;
        Application::framerateCounter = nullptr;
    }
}

void Application::toggleFramerateDisplay()
{
    Application::setDisplayFramerate(!Application::framerateCounter);
}

void Application::resizeFramerateCounter()
{
    if (!Application::framerateCounter)
        return;

    Style* style                   = Application::getStyle();
    unsigned framerateCounterWidth = style->FramerateCounter.width;
    unsigned width                 = WINDOW_WIDTH;

    Application::framerateCounter->setBoundaries(
        width - framerateCounterWidth,
        0,
        framerateCounterWidth,
        style->FramerateCounter.height);
    Application::framerateCounter->invalidate();
}

void Application::resizeNotificationManager()
{
    Application::notificationManager->setBoundaries(0, 0, Application::contentWidth, Application::contentHeight);
    Application::notificationManager->invalidate();
}

void Application::notify(std::string text)
{
    Application::notificationManager->notify(text);
}

NotificationManager* Application::getNotificationManager()
{
    return Application::notificationManager;
}

void Application::giveFocus(View* view)
{
    View* oldFocus = Application::currentFocus;
    View* newFocus = view ? view->getDefaultFocus() : nullptr;

    if (oldFocus != newFocus)
    {
        if (oldFocus)
            oldFocus->onFocusLost();

        Application::currentFocus = newFocus;
        Application::globalFocusChangeEvent.fire(newFocus);

        if (newFocus)
        {
            newFocus->onFocusGained();
            Logger::debug("Giving focus to %s", newFocus->describe().c_str());
        }
    }
}

void Application::popView(ViewAnimation animation, std::function<void(void)> cb)
{
    if (Application::viewStack.size() <= 1) // never pop the root view
        return;

    Application::blockInputs();

    View* last = Application::viewStack[Application::viewStack.size() - 1];
    last->willDisappear(true);

    last->setForceTranslucent(true);

    bool wait = animation == ViewAnimation::FADE; // wait for the new view animation to be done before showing the old one?

    // Hide animation (and show previous view, if any)
    last->hide([last, animation, wait, cb]() {
        last->setForceTranslucent(false);
        Application::viewStack.pop_back();
        delete last;

        // Animate the old view once the new one
        // has ended its animation
        if (Application::viewStack.size() > 0 && wait)
        {
            View* newLast = Application::viewStack[Application::viewStack.size() - 1];

            if (newLast->isHidden())
            {
                newLast->willAppear(false);
                newLast->show(cb, true, animation);
            }
            else
            {
                cb();
            }
        }

        Application::unblockInputs();
    },
        true, animation);

    // Animate the old view immediately
    if (!wait && Application::viewStack.size() > 1)
    {
        View* toShow = Application::viewStack[Application::viewStack.size() - 2];
        toShow->willAppear(false);
        toShow->show(cb, true, animation);
    }

    // Focus
    if (Application::focusStack.size() > 0)
    {
        View* newFocus = Application::focusStack[Application::focusStack.size() - 1];

        Logger::debug("Giving focus to %s, and removing it from the focus stack", newFocus->describe().c_str());

        Application::giveFocus(newFocus);
        Application::focusStack.pop_back();
    }
}

void Application::pushView(View* view, ViewAnimation animation)
{
    Application::blockInputs();

    // Call hide() on the previous view in the stack if no
    // views are translucent, then call show() once the animation ends
    View* last = nullptr;
    if (Application::viewStack.size() > 0)
        last = Application::viewStack[Application::viewStack.size() - 1];

    bool fadeOut = last && !last->isTranslucent() && !view->isTranslucent(); // play the fade out animation?
    bool wait    = animation == ViewAnimation::FADE; // wait for the old view animation to be done before showing the new one?

    view->registerAction("Exit", Key::PLUS, [] { Application::quit(); return true; });
    view->registerAction(
        "FPS", Key::MINUS, [] { Application::toggleFramerateDisplay(); return true; }, true);

    // Fade out animation
    if (fadeOut)
    {
        view->setForceTranslucent(true); // set the new view translucent until the fade out animation is done playing

        // Animate the new view directly
        if (!wait)
        {
            view->show([]() {
                Application::unblockInputs();
            },
                true, animation);
        }

        last->hide([animation, wait]() {
            View* newLast = Application::viewStack[Application::viewStack.size() - 1];
            newLast->setForceTranslucent(false);

            // Animate the new view once the old one
            // has ended its animation
            if (wait)
                newLast->show([]() { Application::unblockInputs(); }, true, animation);
        },
            true, animation);
    }

    view->setBoundaries(0, 0, Application::contentWidth, Application::contentHeight);

    if (!fadeOut)
        view->show([]() { Application::unblockInputs(); }, true, animation);
    else
        view->alpha = 0.0f;

    // Focus
    if (Application::viewStack.size() > 0)
    {
        Logger::debug("Pushing %s to the focus stack", Application::currentFocus->describe().c_str());
        Application::focusStack.push_back(Application::currentFocus);
    }

    // Layout and prepare view
    view->invalidate(true);
    view->willAppear(true);
    Application::giveFocus(view->getDefaultFocus());

    // And push it
    Application::viewStack.push_back(view);
}

void Application::onWindowSizeChanged()
{
    Logger::debug("Layout triggered");

    for (View* view : Application::viewStack)
    {
        view->setBoundaries(0, 0, Application::contentWidth, Application::contentHeight);
        view->invalidate();

        view->onWindowSizeChanged();
    }

    Application::resizeNotificationManager();
    Application::resizeFramerateCounter();
}

void Application::clear()
{
    for (View* view : Application::viewStack)
    {
        view->willDisappear(true);
        delete view;
    }

    Application::viewStack.clear();
}

Style* Application::getStyle()
{
    return &Application::currentStyle;
}

void Application::setTheme(Theme theme)
{
    Application::currentTheme = theme;
}

ThemeValues* Application::getThemeValues()
{
    return &Application::currentTheme.colors[Application::currentThemeVariant];
}

ThemeValues* Application::getThemeValuesForVariant(ThemeVariant variant)
{
    return &Application::currentTheme.colors[variant];
}

ThemeVariant Application::getThemeVariant()
{
    return Application::currentThemeVariant;
}

int Application::loadFont(const char* fontName, const char* filePath)
{
    return nvgCreateFont(Application::vg, fontName, filePath);
}

int Application::loadFontFromMemory(const char* fontName, void* address, size_t size, bool freeData)
{
    return nvgCreateFontMem(Application::vg, fontName, (unsigned char*)address, size, freeData);
}

int Application::findFont(const char* fontName)
{
    return nvgFindFont(Application::vg, fontName);
}

void Application::crash(std::string text)
{
    CrashFrame* crashFrame = new CrashFrame(text);
    Application::pushView(crashFrame);
}

void Application::blockInputs()
{
    Application::blockInputsTokens += 1;
}

void Application::unblockInputs()
{
    if (Application::blockInputsTokens > 0)
        Application::blockInputsTokens -= 1;
}

NVGcontext* Application::getNVGContext()
{
    return Application::vg;
}

TaskManager* Application::getTaskManager()
{
    return Application::taskManager;
}

void Application::setCommonFooter(std::string footer)
{
    Application::commonFooter = footer;
}

std::string* Application::getCommonFooter()
{
    return &Application::commonFooter;
}

FramerateCounter::FramerateCounter()
    : Label(LabelStyle::LIST_ITEM, "FPS: ---")
{
    this->setColor(nvgRGB(255, 255, 255));
    this->setVerticalAlign(NVG_ALIGN_MIDDLE);
    this->setHorizontalAlign(NVG_ALIGN_RIGHT);
    this->setBackground(Background::BACKDROP);

    this->lastSecond = cpu_features_get_time_usec() / 1000;
}

void FramerateCounter::frame(FrameContext* ctx)
{
    // Update counter
    retro_time_t current = cpu_features_get_time_usec() / 1000;

    if (current - this->lastSecond >= 1000)
    {
        char fps[10];
        snprintf(fps, sizeof(fps), "FPS: %03d", this->frames);
        this->setText(std::string(fps));
        this->invalidate(); // update width for background

        this->frames     = 0;
        this->lastSecond = current;
    }

    this->frames++;

    // Regular frame
    Label::frame(ctx);
}

void Application::setMaximumFPS(unsigned fps)
{
    if (fps == 0)
        Application::frameTime = 0.0f;
    else
    {
        Application::frameTime = 1000 / (float)fps;
    }

    Logger::info("Maximum FPS set to %d - using a frame time of %.2f ms", fps, Application::frameTime);
}

std::string Application::getTitle()
{
    return Application::title;
}

GenericEvent* Application::getGlobalFocusChangeEvent()
{
    return &Application::globalFocusChangeEvent;
}

VoidEvent* Application::getGlobalHintsUpdateEvent()
{
    return &Application::globalHintsUpdateEvent;
}

FontStash* Application::getFontStash()
{
    return &Application::fontStash;
}

} // namespace brls
