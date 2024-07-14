#include "imgui-SFML.h"
#include <imgui.h>

#include <SFML/Config.hpp>
#include <SFML/Graphics/Color.hpp>
#include <SFML/Graphics/RenderTarget.hpp>
#include <SFML/Graphics/RenderTexture.hpp>
#include <SFML/Graphics/RenderWindow.hpp>
#include <SFML/Graphics/Sprite.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <SFML/OpenGL.hpp>
#include <SFML/System/String.hpp>
#include <SFML/System/StringUtfUtils.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <SFML/Window/Cursor.hpp>
#include <SFML/Window/Event.hpp>
#include <SFML/Window/Touch.hpp>
#include <SFML/Window/Window.hpp>

#include <cassert>
#include <cmath> // abs
#include <cstring> // memcpy

#include <algorithm>
#include <memory>
#include <vector>

#if defined(__APPLE__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#ifdef ANDROID
#ifdef USE_JNI

#include <SFML/System/NativeActivity.hpp>
#include <android/native_activity.h>
#include <jni.h>

int openKeyboardIME() {
    ANativeActivity* activity = sf::getNativeActivity();
    JavaVM* vm = activity->vm;
    JNIEnv* env = activity->env;
    JavaVMAttachArgs attachargs;
    attachargs.version = JNI_VERSION_1_6;
    attachargs.name = "NativeThread";
    attachargs.group = nullptr;
    jint res = vm->AttachCurrentThread(&env, &attachargs);
    if (res == JNI_ERR) return EXIT_FAILURE;

    jclass natact = env->FindClass("android/app/NativeActivity");
    jclass context = env->FindClass("android/content/Context");

    jfieldID fid = env->GetStaticFieldID(context, "INPUT_METHOD_SERVICE", "Ljava/lang/String;");
    jobject svcstr = env->GetStaticObjectField(context, fid);

    jmethodID getss =
        env->GetMethodID(natact, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject imm_obj = env->CallObjectMethod(activity->clazz, getss, svcstr);

    jclass imm_cls = env->GetObjectClass(imm_obj);
    jmethodID toggleSoftInput = env->GetMethodID(imm_cls, "toggleSoftInput", "(II)V");

    env->CallVoidMethod(imm_obj, toggleSoftInput, 2, 0);

    env->DeleteLocalRef(imm_obj);
    env->DeleteLocalRef(imm_cls);
    env->DeleteLocalRef(svcstr);
    env->DeleteLocalRef(context);
    env->DeleteLocalRef(natact);

    vm->DetachCurrentThread();

    return EXIT_SUCCESS;
}

int closeKeyboardIME() {
    ANativeActivity* activity = sf::getNativeActivity();
    JavaVM* vm = activity->vm;
    JNIEnv* env = activity->env;
    JavaVMAttachArgs attachargs;
    attachargs.version = JNI_VERSION_1_6;
    attachargs.name = "NativeThread";
    attachargs.group = nullptr;
    jint res = vm->AttachCurrentThread(&env, &attachargs);
    if (res == JNI_ERR) return EXIT_FAILURE;

    jclass natact = env->FindClass("android/app/NativeActivity");
    jclass context = env->FindClass("android/content/Context");

    jfieldID fid = env->GetStaticFieldID(context, "INPUT_METHOD_SERVICE", "Ljava/lang/String;");
    jobject svcstr = env->GetStaticObjectField(context, fid);

    jmethodID getss =
        env->GetMethodID(natact, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject imm_obj = env->CallObjectMethod(activity->clazz, getss, svcstr);

    jclass imm_cls = env->GetObjectClass(imm_obj);
    jmethodID toggleSoftInput = env->GetMethodID(imm_cls, "toggleSoftInput", "(II)V");

    env->CallVoidMethod(imm_obj, toggleSoftInput, 1, 0);

    env->DeleteLocalRef(imm_obj);
    env->DeleteLocalRef(imm_cls);
    env->DeleteLocalRef(svcstr);
    env->DeleteLocalRef(context);
    env->DeleteLocalRef(natact);

    vm->DetachCurrentThread();

    return EXIT_SUCCESS;
}

#endif
#endif

static_assert(sizeof(GLuint) <= sizeof(ImTextureID),
              "ImTextureID is not large enough to fit GLuint.");

namespace {
// various helper functions
ImColor toImColor(sf::Color c);
ImVec2 toImVec2(const sf::Vector2f& v);
sf::Vector2f toSfVector2f(const ImVec2& v);
ImVec2 getTopLeftAbsolute(const sf::FloatRect& rect);
ImVec2 getDownRightAbsolute(const sf::FloatRect& rect);

struct SpriteTextureData {
    ImVec2 uv0;
    ImVec2 uv1;
    ImTextureID textureID{};
};

SpriteTextureData getSpriteTextureData(const sf::Sprite& sprite, const sf::Texture& texture);

ImTextureID convertGLTextureHandleToImTextureID(GLuint glTextureHandle);
GLuint convertImTextureIDToGLTextureHandle(ImTextureID textureID);

void RenderDrawLists(ImDrawData* draw_data); // rendering callback function prototype

// Default mapping is XInput gamepad mapping
void initDefaultJoystickMapping();

// Returns first id of connected joystick
unsigned int getConnectedJoystickId();

void updateJoystickButtonState(ImGuiIO& io);
void updateJoystickDPadState(ImGuiIO& io);
void updateJoystickAxisState(ImGuiIO& io);

// clipboard functions
void setClipboardText(void* userData, const char* text);
const char* getClipboardText(void* userData);
std::string s_clipboardText;

// mouse cursors
void loadMouseCursor(ImGuiMouseCursor imguiCursorType, sf::Cursor::Type sfmlCursorType);
void updateMouseCursor(sf::Window& window);

// Key mappings
ImGuiKey keycodeToImGuiKey(sf::Keyboard::Key code);
ImGuiKey keycodeToImGuiMod(sf::Keyboard::Key code);

// data
constexpr unsigned int NULL_JOYSTICK_ID = sf::Joystick::Count;

struct StickInfo {
    sf::Joystick::Axis xAxis{sf::Joystick::Axis::X};
    sf::Joystick::Axis yAxis{sf::Joystick::Axis::Y};

    bool xInverted{false};
    bool yInverted{false};

    float threshold{15};
};

struct TriggerInfo {
    sf::Joystick::Axis axis{sf::Joystick::Axis::Z};
    float threshold{0};
};

struct WindowContext {
    const sf::Window* window;
    ImGuiContext* imContext{ImGui::CreateContext()};

    sf::Optional<sf::Texture> fontTexture; // internal font atlas which is used if user doesn't set
                                           // a custom sf::Texture.

    bool windowHasFocus;
    bool mouseMoved{false};
    bool mousePressed[3] = {false};
    ImGuiMouseCursor lastCursor{ImGuiMouseCursor_COUNT};

    bool touchDown[3] = {false};
    sf::Vector2i touchPos;

    unsigned int joystickId{getConnectedJoystickId()};
    ImGuiKey joystickMapping[sf::Joystick::ButtonCount] = {ImGuiKey_None};
    StickInfo dPadInfo;
    StickInfo lStickInfo;
    StickInfo rStickInfo;
    TriggerInfo lTriggerInfo;
    TriggerInfo rTriggerInfo;

    sf::Optional<sf::Cursor> mouseCursors[ImGuiMouseCursor_COUNT];

#ifdef ANDROID
#ifdef USE_JNI
    bool wantTextInput{false};
#endif
#endif

    WindowContext(const sf::Window* w) : window(w), windowHasFocus(window->hasFocus()) {}
    ~WindowContext() { ImGui::DestroyContext(imContext); }

    WindowContext(const WindowContext&) = delete; // non construction-copyable
    WindowContext& operator=(const WindowContext&) = delete; // non copyable
};

std::vector<std::unique_ptr<WindowContext>> s_windowContexts;
WindowContext* s_currWindowCtx = nullptr;

} // end of anonymous namespace

namespace ImGui {
namespace SFML {
bool Init(sf::GraphicsContext& graphicsContext, sf::RenderWindow& window, bool loadDefaultFont) {
    return Init(graphicsContext, window, window, loadDefaultFont);
}

bool Init(sf::GraphicsContext& graphicsContext, sf::Window& window, sf::RenderTarget& target,
          bool loadDefaultFont) {
    return Init(graphicsContext, window, target.getSize().to<sf::Vector2f>(), loadDefaultFont);
}

bool Init(sf::GraphicsContext& graphicsContext, sf::Window& window, const sf::Vector2f& displaySize,
          bool loadDefaultFont) {
    s_windowContexts.emplace_back(std::make_unique<WindowContext>(&window));

    s_currWindowCtx = s_windowContexts.back().get();
    ImGui::SetCurrentContext(s_currWindowCtx->imContext);

    ImGuiIO& io = ImGui::GetIO();

    // tell ImGui which features we support
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendFlags |= ImGuiBackendFlags_HasSetMousePos;
    io.BackendPlatformName = "imgui_impl_sfml";

    s_currWindowCtx->joystickId = getConnectedJoystickId();

    initDefaultJoystickMapping();

    // init rendering
    io.DisplaySize = toImVec2(displaySize);

    // clipboard
    io.SetClipboardTextFn = setClipboardText;
    io.GetClipboardTextFn = getClipboardText;

    // load mouse cursors
    loadMouseCursor(ImGuiMouseCursor_Arrow, sf::Cursor::Type::Arrow);
    loadMouseCursor(ImGuiMouseCursor_TextInput, sf::Cursor::Type::Text);
    loadMouseCursor(ImGuiMouseCursor_ResizeAll, sf::Cursor::Type::SizeAll);
    loadMouseCursor(ImGuiMouseCursor_ResizeNS, sf::Cursor::Type::SizeVertical);
    loadMouseCursor(ImGuiMouseCursor_ResizeEW, sf::Cursor::Type::SizeHorizontal);
    loadMouseCursor(ImGuiMouseCursor_ResizeNESW, sf::Cursor::Type::SizeBottomLeftTopRight);
    loadMouseCursor(ImGuiMouseCursor_ResizeNWSE, sf::Cursor::Type::SizeTopLeftBottomRight);
    loadMouseCursor(ImGuiMouseCursor_Hand, sf::Cursor::Type::Hand);

    if (loadDefaultFont) {
        // this will load default font automatically
        // No need to call AddDefaultFont
        return UpdateFontTexture(graphicsContext);
    }

    return true;
}

void SetCurrentWindow(const sf::Window& window) {
    auto found = std::find_if(s_windowContexts.begin(), s_windowContexts.end(),
                              [&](std::unique_ptr<WindowContext>& ctx) {
                                  return ctx->window->getNativeHandle() == window.getNativeHandle();
                              });
    assert(found != s_windowContexts.end() &&
           "Failed to find the window. Forgot to call ImGui::SFML::Init for the window?");
    s_currWindowCtx = found->get();
    ImGui::SetCurrentContext(s_currWindowCtx->imContext);
}

void ProcessEvent(const sf::Window& window, const sf::Event& event) {
    SetCurrentWindow(window);
    assert(s_currWindowCtx && "No current window is set - forgot to call ImGui::SFML::Init?");
    ImGuiIO& io = ImGui::GetIO();

    if (s_currWindowCtx->windowHasFocus) {
        if (const auto* resized = event.getIf<sf::Event::Resized>()) {
            io.DisplaySize =
                ImVec2(static_cast<float>(resized->size.x), static_cast<float>(resized->size.y));
        } else if (const auto* mouseMoved = event.getIf<sf::Event::MouseMoved>()) {
            io.AddMousePosEvent(static_cast<float>(mouseMoved->position.x),
                                static_cast<float>(mouseMoved->position.y));
            s_currWindowCtx->mouseMoved = true;
        } else if (const auto* mouseButtonPressed = event.getIf<sf::Event::MouseButtonPressed>()) {
            const int button = static_cast<int>(mouseButtonPressed->button);
            if (button >= 0 && button < 3) {
                s_currWindowCtx->mousePressed[static_cast<int>(mouseButtonPressed->button)] = true;
                io.AddMouseButtonEvent(button, true);
            }
        } else if (const auto* mouseButtonReleased =
                       event.getIf<sf::Event::MouseButtonReleased>()) {
            const int button = static_cast<int>(mouseButtonReleased->button);
            if (button >= 0 && button < 3) io.AddMouseButtonEvent(button, false);
        } else if (const auto* touchBegan = event.getIf<sf::Event::TouchBegan>()) {
            s_currWindowCtx->mouseMoved = false;
            const unsigned int button = touchBegan->finger;
            if (button < 3) s_currWindowCtx->touchDown[touchBegan->finger] = true;
        } else if (event.is<sf::Event::TouchEnded>()) {
            s_currWindowCtx->mouseMoved = false;
        } else if (const auto* mouseWheelScrolled = event.getIf<sf::Event::MouseWheelScrolled>()) {
            if (mouseWheelScrolled->wheel == sf::Mouse::Wheel::Vertical ||
                (mouseWheelScrolled->wheel == sf::Mouse::Wheel::Horizontal && io.KeyShift)) {
                io.AddMouseWheelEvent(0, mouseWheelScrolled->delta);
            } else if (mouseWheelScrolled->wheel == sf::Mouse::Wheel::Horizontal) {
                io.AddMouseWheelEvent(mouseWheelScrolled->delta, 0);
            }
        }
        const auto handleKeyChanged = [&io](const auto& keyChanged, bool down) {
            const ImGuiKey mod = keycodeToImGuiMod(keyChanged.code);
            // The modifier booleans are not reliable when it's the modifier
            // itself that's being pressed. Detect these presses directly.
            if (mod != ImGuiKey_None) {
                io.AddKeyEvent(mod, down);
            } else {
                io.AddKeyEvent(ImGuiKey_ModCtrl, keyChanged.control);
                io.AddKeyEvent(ImGuiKey_ModShift, keyChanged.shift);
                io.AddKeyEvent(ImGuiKey_ModAlt, keyChanged.alt);
                io.AddKeyEvent(ImGuiKey_ModSuper, keyChanged.system);
            }

            const ImGuiKey key = keycodeToImGuiKey(keyChanged.code);
            io.AddKeyEvent(key, down);
            io.SetKeyEventNativeData(key, static_cast<int>(keyChanged.code), -1);
        };
        if (const auto* keyPressed = event.getIf<sf::Event::KeyPressed>()) {
            handleKeyChanged(*keyPressed, true);
        } else if (const auto* keyReleased = event.getIf<sf::Event::KeyReleased>()) {
            handleKeyChanged(*keyReleased, false);
        } else if (const auto* textEntered = event.getIf<sf::Event::TextEntered>()) {
            // Don't handle the event for unprintable characters
            if (!(textEntered->unicode < ' ' || textEntered->unicode == 127))
                io.AddInputCharacter(textEntered->unicode);
        } else if (const auto* joystickConnected = event.getIf<sf::Event::JoystickConnected>()) {
            if (s_currWindowCtx->joystickId == NULL_JOYSTICK_ID)
                s_currWindowCtx->joystickId = joystickConnected->joystickId;
        } else if (const auto* joystickDisconnected =
                       event.getIf<sf::Event::JoystickDisconnected>()) {
            if (s_currWindowCtx->joystickId == joystickDisconnected->joystickId)
                // used gamepad was disconnected
                s_currWindowCtx->joystickId = getConnectedJoystickId();
        }
    }

    if (event.is<sf::Event::FocusLost>()) {
        io.AddFocusEvent(false);
        s_currWindowCtx->windowHasFocus = false;
    }
    if (event.is<sf::Event::FocusGained>()) {
        io.AddFocusEvent(true);
        s_currWindowCtx->windowHasFocus = true;
    }
}

void Update(sf::RenderWindow& window, sf::Time dt) {
    Update(window, window, dt);
}

void Update(sf::Window& window, sf::RenderTarget& target, sf::Time dt) {
    SetCurrentWindow(window);
    assert(s_currWindowCtx);

    // Update OS/hardware mouse cursor if imgui isn't drawing a software cursor
    const ImGuiMouseCursor mouse_cursor =
        ImGui::GetIO().MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
    if (s_currWindowCtx->lastCursor != mouse_cursor) {
        s_currWindowCtx->lastCursor = mouse_cursor;
        updateMouseCursor(window);
    }

    if (!s_currWindowCtx->mouseMoved) {
        if (sf::Touch::isDown(0)) s_currWindowCtx->touchPos = sf::Touch::getPosition(0, window);

        Update(s_currWindowCtx->touchPos, target.getSize().to<sf::Vector2f>(), dt);
    } else {
        Update(sf::Mouse::getPosition(window), target.getSize().to<sf::Vector2f>(), dt);
    }
}

void Update(const sf::Vector2i& mousePos, const sf::Vector2f& displaySize, sf::Time dt) {
    assert(s_currWindowCtx && "No current window is set - forgot to call ImGui::SFML::Init?");

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = toImVec2(displaySize);
    io.DeltaTime = dt.asSeconds();

    if (s_currWindowCtx->windowHasFocus) {
        if (io.WantSetMousePos) {
            const sf::Vector2i newMousePos(static_cast<int>(io.MousePos.x),
                                           static_cast<int>(io.MousePos.y));
            sf::Mouse::setPosition(newMousePos);
        } else {
            io.MousePos = ImVec2(static_cast<float>(mousePos.x), static_cast<float>(mousePos.y));
        }
        for (unsigned int i = 0; i < 3; i++) {
            io.MouseDown[i] = s_currWindowCtx->touchDown[i] || sf::Touch::isDown(i) ||
                              s_currWindowCtx->mousePressed[i] ||
                              sf::Mouse::isButtonPressed((sf::Mouse::Button)i);
            s_currWindowCtx->mousePressed[i] = false;
            s_currWindowCtx->touchDown[i] = false;
        }
    }

#ifdef ANDROID
#ifdef USE_JNI
    if (io.WantTextInput && !s_currWindowCtx->wantTextInput) {
        openKeyboardIME();
        s_currWindowCtx->wantTextInput = true;
    }

    if (!io.WantTextInput && s_currWindowCtx->wantTextInput) {
        closeKeyboardIME();
        s_currWindowCtx->wantTextInput = false;
    }
#endif
#endif

    assert(io.Fonts->Fonts.Size > 0); // You forgot to create and set up font
                                      // atlas (see createFontTexture)

    // gamepad navigation
    if ((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) &&
        s_currWindowCtx->joystickId != NULL_JOYSTICK_ID) {
        updateJoystickButtonState(io);
        updateJoystickDPadState(io);
        updateJoystickAxisState(io);
    }

    ImGui::NewFrame();
}

void Render(sf::RenderWindow& window) {
    SetCurrentWindow(window);
    Render(static_cast<sf::RenderTarget&>(window));
}

void Render(sf::RenderTarget& target) {
    target.resetGLStates();
    target.pushGLStates();
    ImGui::Render();
    RenderDrawLists(ImGui::GetDrawData());
    target.popGLStates();
}

void Render() {
    ImGui::Render();
    RenderDrawLists(ImGui::GetDrawData());
}

void Shutdown(const sf::Window& window) {
    const bool needReplacement =
        (s_currWindowCtx->window->getNativeHandle() == window.getNativeHandle());

    // remove window's context
    auto found = std::find_if(s_windowContexts.begin(), s_windowContexts.end(),
                              [&](std::unique_ptr<WindowContext>& ctx) {
                                  return ctx->window->getNativeHandle() == window.getNativeHandle();
                              });
    assert(found != s_windowContexts.end() &&
           "Window wasn't inited properly: forgot to call ImGui::SFML::Init(window)?");
    s_windowContexts.erase(found); // s_currWindowCtx can become invalid here!

    // set current context to some window for convenience if needed
    if (needReplacement) {
        auto it = s_windowContexts.begin();
        if (it != s_windowContexts.end()) {
            // set to some other window
            s_currWindowCtx = it->get();
            ImGui::SetCurrentContext(s_currWindowCtx->imContext);
        } else {
            // no alternatives...
            s_currWindowCtx = nullptr;
            ImGui::SetCurrentContext(nullptr);
        }
    }
}

void Shutdown() {
    s_currWindowCtx = nullptr;
    ImGui::SetCurrentContext(nullptr);

    s_windowContexts.clear();
}

bool UpdateFontTexture(sf::GraphicsContext& graphicsContext) {
    assert(s_currWindowCtx);

    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;

    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    auto newTexture = sf::Texture::create(graphicsContext, {static_cast<unsigned>(width),
                                                            static_cast<unsigned>(height)});

    if (!newTexture.hasValue()) {
        return false;
    }

    newTexture->update(pixels);

    ImTextureID texID = convertGLTextureHandleToImTextureID(newTexture->getNativeHandle());
    io.Fonts->SetTexID(texID);

    s_currWindowCtx->fontTexture = std::move(newTexture);

    return true;
}

sf::Optional<sf::Texture>& GetFontTexture() {
    assert(s_currWindowCtx);
    return s_currWindowCtx->fontTexture;
}

void SetActiveJoystickId(unsigned int joystickId) {
    assert(s_currWindowCtx);
    assert(joystickId < sf::Joystick::Count);
    s_currWindowCtx->joystickId = joystickId;
}

void SetJoystickDPadThreshold(float threshold) {
    assert(s_currWindowCtx);
    assert(threshold >= 0.f && threshold <= 100.f);
    s_currWindowCtx->dPadInfo.threshold = threshold;
}

void SetJoystickLStickThreshold(float threshold) {
    assert(s_currWindowCtx);
    assert(threshold >= 0.f && threshold <= 100.f);
    s_currWindowCtx->lStickInfo.threshold = threshold;
}

void SetJoystickRStickThreshold(float threshold) {
    assert(s_currWindowCtx);
    assert(threshold >= 0.f && threshold <= 100.f);
    s_currWindowCtx->rStickInfo.threshold = threshold;
}

void SetJoystickLTriggerThreshold(float threshold) {
    assert(s_currWindowCtx);
    assert(threshold >= -100.f && threshold <= 100.f);
    s_currWindowCtx->lTriggerInfo.threshold = threshold;
}

void SetJoystickRTriggerThreshold(float threshold) {
    assert(s_currWindowCtx);
    assert(threshold >= -100.f && threshold <= 100.f);
    s_currWindowCtx->rTriggerInfo.threshold = threshold;
}

void SetJoystickMapping(int key, unsigned int joystickButton) {
    assert(s_currWindowCtx);
    // This function now expects ImGuiKey_* values.
    // For partial backwards compatibility, also expect some ImGuiNavInput_* values.
    ImGuiKey finalKey{};
    switch (key) {
    case ImGuiNavInput_Activate:
        finalKey = ImGuiKey_GamepadFaceDown;
        break;
    case ImGuiNavInput_Cancel:
        finalKey = ImGuiKey_GamepadFaceRight;
        break;
    case ImGuiNavInput_Input:
        finalKey = ImGuiKey_GamepadFaceUp;
        break;
    case ImGuiNavInput_Menu:
        finalKey = ImGuiKey_GamepadFaceLeft;
        break;
    case ImGuiNavInput_FocusPrev:
    case ImGuiNavInput_TweakSlow:
        finalKey = ImGuiKey_GamepadL1;
        break;
    case ImGuiNavInput_FocusNext:
    case ImGuiNavInput_TweakFast:
        finalKey = ImGuiKey_GamepadR1;
        break;
    default:
        assert(key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END);
        finalKey = static_cast<ImGuiKey>(key);
    }
    assert(joystickButton < sf::Joystick::ButtonCount);
    s_currWindowCtx->joystickMapping[joystickButton] = finalKey;
}

void SetDPadXAxis(sf::Joystick::Axis dPadXAxis, bool inverted) {
    assert(s_currWindowCtx);
    s_currWindowCtx->dPadInfo.xAxis = dPadXAxis;
    s_currWindowCtx->dPadInfo.xInverted = inverted;
}

void SetDPadYAxis(sf::Joystick::Axis dPadYAxis, bool inverted) {
    assert(s_currWindowCtx);
    s_currWindowCtx->dPadInfo.yAxis = dPadYAxis;
    s_currWindowCtx->dPadInfo.yInverted = inverted;
}

void SetLStickXAxis(sf::Joystick::Axis lStickXAxis, bool inverted) {
    assert(s_currWindowCtx);
    s_currWindowCtx->lStickInfo.xAxis = lStickXAxis;
    s_currWindowCtx->lStickInfo.xInverted = inverted;
}

void SetLStickYAxis(sf::Joystick::Axis lStickYAxis, bool inverted) {
    assert(s_currWindowCtx);
    s_currWindowCtx->lStickInfo.yAxis = lStickYAxis;
    s_currWindowCtx->lStickInfo.yInverted = inverted;
}

void SetRStickXAxis(sf::Joystick::Axis rStickXAxis, bool inverted) {
    assert(s_currWindowCtx);
    s_currWindowCtx->rStickInfo.xAxis = rStickXAxis;
    s_currWindowCtx->rStickInfo.xInverted = inverted;
}

void SetRStickYAxis(sf::Joystick::Axis rStickYAxis, bool inverted) {
    assert(s_currWindowCtx);
    s_currWindowCtx->rStickInfo.yAxis = rStickYAxis;
    s_currWindowCtx->rStickInfo.yInverted = inverted;
}

void SetLTriggerAxis(sf::Joystick::Axis lTriggerAxis) {
    assert(s_currWindowCtx);
    s_currWindowCtx->rTriggerInfo.axis = lTriggerAxis;
}

void SetRTriggerAxis(sf::Joystick::Axis rTriggerAxis) {
    assert(s_currWindowCtx);
    s_currWindowCtx->rTriggerInfo.axis = rTriggerAxis;
}

} // end of namespace SFML

/////////////// Image Overloads for sf::Texture

void Image(const sf::Texture& texture, const sf::Color& tintColor, const sf::Color& borderColor) {
    Image(texture, texture.getSize().to<sf::Vector2f>(), tintColor, borderColor);
}

void Image(const sf::Texture& texture, const sf::Vector2f& size, const sf::Color& tintColor,
           const sf::Color& borderColor) {
    ImTextureID textureID = convertGLTextureHandleToImTextureID(texture.getNativeHandle());

    ImGui::Image(textureID, toImVec2(size), ImVec2(0, 0), ImVec2(1, 1), toImColor(tintColor),
                 toImColor(borderColor));
}

/////////////// Image Overloads for sf::RenderTexture
void Image(const sf::RenderTexture& texture, const sf::Color& tintColor,
           const sf::Color& borderColor) {
    Image(texture, texture.getSize().to<sf::Vector2f>(), tintColor, borderColor);
}

void Image(const sf::RenderTexture& texture, const sf::Vector2f& size, const sf::Color& tintColor,
           const sf::Color& borderColor) {
    ImTextureID textureID =
        convertGLTextureHandleToImTextureID(texture.getTexture().getNativeHandle());

    ImGui::Image(textureID, toImVec2(size), ImVec2(0, 1), ImVec2(1, 0), // flipped vertically,
                                                                        // because textures in
                                                                        // sf::RenderTexture are
                                                                        // stored this way
                 toImColor(tintColor), toImColor(borderColor));
}

/////////////// Image Overloads for sf::Sprite

void Image(const sf::Sprite& sprite, const sf::Texture& texture, const sf::Color& tintColor,
           const sf::Color& borderColor) {
    Image(sprite, texture, sprite.getGlobalBounds().size, tintColor, borderColor);
}

void Image(const sf::Sprite& sprite, const sf::Texture& texture, const sf::Vector2f& size,
           const sf::Color& tintColor, const sf::Color& borderColor) {
    auto [uv0, uv1, textureID] = getSpriteTextureData(sprite, texture);
    ImGui::Image(textureID, toImVec2(size), uv0, uv1, toImColor(tintColor), toImColor(borderColor));
}

/////////////// Image Button Overloads for sf::Texture

bool ImageButton(const char* id, const sf::Texture& texture, const sf::Vector2f& size,
                 const sf::Color& bgColor, const sf::Color& tintColor) {
    ImTextureID textureID = convertGLTextureHandleToImTextureID(texture.getNativeHandle());

    return ImGui::ImageButton(id, textureID, toImVec2(size), ImVec2(0, 0), ImVec2(1, 1),
                              toImColor(bgColor), toImColor(tintColor));
}

/////////////// Image Button Overloads for sf::RenderTexture

bool ImageButton(const char* id, const sf::RenderTexture& texture, const sf::Vector2f& size,
                 const sf::Color& bgColor, const sf::Color& tintColor) {
    ImTextureID textureID =
        convertGLTextureHandleToImTextureID(texture.getTexture().getNativeHandle());

    return ImGui::ImageButton(id, textureID, toImVec2(size), ImVec2(0, 1),
                              ImVec2(1, 0), // flipped vertically, because textures in
                                            // sf::RenderTexture are stored this way
                              toImColor(bgColor), toImColor(tintColor));
}

/////////////// Image Button Overloads for sf::Sprite

bool ImageButton(const char* id, const sf::Sprite& sprite, const sf::Texture& texture,
                 const sf::Vector2f& size, const sf::Color& bgColor, const sf::Color& tintColor) {
    auto [uv0, uv1, textureID] = getSpriteTextureData(sprite, texture);
    return ImGui::ImageButton(id, textureID, toImVec2(size), uv0, uv1, toImColor(bgColor),
                              toImColor(tintColor));
}

/////////////// Draw_list Overloads

void DrawLine(const sf::Vector2f& a, const sf::Vector2f& b, const sf::Color& color,
              float thickness) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    draw_list->AddLine(ImVec2(a.x + pos.x, a.y + pos.y), ImVec2(b.x + pos.x, b.y + pos.y),
                       ColorConvertFloat4ToU32(toImColor(color)), thickness);
}

void DrawRect(const sf::FloatRect& rect, const sf::Color& color, float rounding,
              int rounding_corners, float thickness) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRect(getTopLeftAbsolute(rect), getDownRightAbsolute(rect),
                       ColorConvertFloat4ToU32(toImColor(color)), rounding, rounding_corners,
                       thickness);
}

void DrawRectFilled(const sf::FloatRect& rect, const sf::Color& color, float rounding,
                    int rounding_corners) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(getTopLeftAbsolute(rect), getDownRightAbsolute(rect),
                             ColorConvertFloat4ToU32(toImColor(color)), rounding, rounding_corners);
}

} // end of namespace ImGui

namespace {
ImColor toImColor(sf::Color c) {
    return {static_cast<int>(c.r), static_cast<int>(c.g), static_cast<int>(c.b),
            static_cast<int>(c.a)};
}
ImVec2 toImVec2(const sf::Vector2f& v) {
    return {v.x, v.y};
}
sf::Vector2f toSfVector2f(const ImVec2& v) {
    return {v.x, v.y};
}
ImVec2 getTopLeftAbsolute(const sf::FloatRect& rect) {
    return toImVec2(toSfVector2f(ImGui::GetCursorScreenPos()) + rect.position);
}
ImVec2 getDownRightAbsolute(const sf::FloatRect& rect) {
    return toImVec2(toSfVector2f(ImGui::GetCursorScreenPos()) + rect.position + rect.size);
}

SpriteTextureData getSpriteTextureData(const sf::Sprite& sprite, const sf::Texture& texture) {
    const auto textureSize(texture.getSize().to<sf::Vector2f>());
    const auto textureRect(sprite.getTextureRect().to<sf::FloatRect>());

    return {toImVec2(textureRect.position.cwiseDiv(textureSize)),
            toImVec2((textureRect.position + textureRect.size).cwiseDiv(textureSize)),
            convertGLTextureHandleToImTextureID(texture.getNativeHandle())};
}

ImTextureID convertGLTextureHandleToImTextureID(GLuint glTextureHandle) {
    ImTextureID textureID = nullptr;
    std::memcpy(&textureID, &glTextureHandle, sizeof(GLuint));
    return textureID;
}
GLuint convertImTextureIDToGLTextureHandle(ImTextureID textureID) {
    GLuint glTextureHandle = 0;
    std::memcpy(&glTextureHandle, &textureID, sizeof(GLuint));
    return glTextureHandle;
}

// copied from imgui/backends/imgui_impl_opengl2.cpp
void SetupRenderState(ImDrawData* draw_data, int fb_width, int fb_height) {
    // Setup render state: alpha-blending enabled, no face culling, no depth testing, scissor
    // enabled, vertex/texcoord/color pointers, polygon fill.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    // glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); //
    // In order to composite our output buffer we need to preserve alpha
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    glEnable(GL_SCISSOR_TEST);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glEnable(GL_TEXTURE_2D);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_SMOOTH);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    // Setup viewport, orthographic projection matrix
    // Our visible imgui space lies from draw_data->DisplayPos (top left) to
    // draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single
    // viewport apps.
    glViewport(0, 0, (GLsizei)fb_width, (GLsizei)fb_height);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
#ifdef GL_VERSION_ES_CL_1_1
    glOrthof(draw_data->DisplayPos.x, draw_data->DisplayPos.x + draw_data->DisplaySize.x,
             draw_data->DisplayPos.y + draw_data->DisplaySize.y, draw_data->DisplayPos.y, -1.0f,
             +1.0f);
#else
    glOrtho(draw_data->DisplayPos.x, draw_data->DisplayPos.x + draw_data->DisplaySize.x,
            draw_data->DisplayPos.y + draw_data->DisplaySize.y, draw_data->DisplayPos.y, -1.0f,
            +1.0f);
#endif
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
}

// Rendering callback
void RenderDrawLists(ImDrawData* draw_data) {
    ImGui::GetDrawData();
    if (draw_data->CmdListsCount == 0) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    assert(io.Fonts->TexID != (ImTextureID) nullptr); // You forgot to create and set font texture

    // Avoid rendering when minimized, scale coordinates for retina displays (screen coordinates !=
    // framebuffer coordinates)
    const int fb_width = (int)(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    const int fb_height = (int)(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    if (fb_width == 0 || fb_height == 0) return;
    draw_data->ScaleClipRects(io.DisplayFramebufferScale);

    // Backup GL state
    // Backup GL state
    GLint last_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
    GLint last_polygon_mode[2];
    glGetIntegerv(GL_POLYGON_MODE, last_polygon_mode);
    GLint last_viewport[4];
    glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLint last_scissor_box[4];
    glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box);
    GLint last_shade_model = 0;
    glGetIntegerv(GL_SHADE_MODEL, &last_shade_model);
    GLint last_tex_env_mode = 0;
    glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &last_tex_env_mode);

#ifdef GL_VERSION_ES_CL_1_1
    GLint last_array_buffer;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_element_array_buffer;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer);
#else
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT);
#endif

    // Setup desired GL state
    SetupRenderState(draw_data, fb_width, fb_height);

    // Will project scissor/clipping rectangles into framebuffer space
    const ImVec2 clip_off = draw_data->DisplayPos; // (0,0) unless using multi-viewports
    const ImVec2 clip_scale = draw_data->FramebufferScale; // (1,1) unless using retina display
                                                           // which are often (2,2)

    // Render command lists
    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        const ImDrawVert* vtx_buffer = cmd_list->VtxBuffer.Data;
        const ImDrawIdx* idx_buffer = cmd_list->IdxBuffer.Data;
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                        (const GLvoid*)((const char*)vtx_buffer + IM_OFFSETOF(ImDrawVert, pos)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                          (const GLvoid*)((const char*)vtx_buffer + IM_OFFSETOF(ImDrawVert, uv)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                       (const GLvoid*)((const char*)vtx_buffer + IM_OFFSETOF(ImDrawVert, col)));

        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++) {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback) {
                // User callback, registered via ImDrawList::AddCallback()
                // (ImDrawCallback_ResetRenderState is a special callback value used by the user to
                // request the renderer to reset render state.)
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                    SetupRenderState(draw_data, fb_width, fb_height);
                else
                    pcmd->UserCallback(cmd_list, pcmd);
            } else {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec4 clip_rect;
                clip_rect.x = (pcmd->ClipRect.x - clip_off.x) * clip_scale.x;
                clip_rect.y = (pcmd->ClipRect.y - clip_off.y) * clip_scale.y;
                clip_rect.z = (pcmd->ClipRect.z - clip_off.x) * clip_scale.x;
                clip_rect.w = (pcmd->ClipRect.w - clip_off.y) * clip_scale.y;

                if (clip_rect.x < static_cast<float>(fb_width) &&
                    clip_rect.y < static_cast<float>(fb_height) && clip_rect.z >= 0.0f &&
                    clip_rect.w >= 0.0f) {
                    // Apply scissor/clipping rectangle
                    glScissor((int)clip_rect.x, (int)(static_cast<float>(fb_height) - clip_rect.w),
                              (int)(clip_rect.z - clip_rect.x), (int)(clip_rect.w - clip_rect.y));

                    // Bind texture, Draw
                    const GLuint textureHandle =
                        convertImTextureIDToGLTextureHandle(pcmd->TextureId);
                    glBindTexture(GL_TEXTURE_2D, textureHandle);
                    glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
                                   sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                   idx_buffer + pcmd->IdxOffset);
                }
            }
        }
    }

    // Restore modified GL state
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glBindTexture(GL_TEXTURE_2D, (GLuint)last_texture);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
    glPolygonMode(GL_FRONT, (GLenum)last_polygon_mode[0]);
    glPolygonMode(GL_BACK, (GLenum)last_polygon_mode[1]);
    glViewport(last_viewport[0], last_viewport[1], (GLsizei)last_viewport[2],
               (GLsizei)last_viewport[3]);
    glScissor(last_scissor_box[0], last_scissor_box[1], (GLsizei)last_scissor_box[2],
              (GLsizei)last_scissor_box[3]);
    glShadeModel((GLenum)last_shade_model);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, last_tex_env_mode);

#ifdef GL_VERSION_ES_CL_1_1
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_array_buffer);
    glDisable(GL_SCISSOR_TEST);
#endif
}

unsigned int getConnectedJoystickId() {
    for (unsigned int i = 0; i < (unsigned int)sf::Joystick::Count; ++i) {
        if (sf::Joystick::isConnected(i)) return i;
    }

    return NULL_JOYSTICK_ID;
}

void initDefaultJoystickMapping() {
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadFaceDown, 0);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadFaceRight, 1);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadFaceLeft, 2);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadFaceUp, 3);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadL1, 4);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadR1, 5);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadBack, 6);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadStart, 7);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadL3, 9);
    ImGui::SFML::SetJoystickMapping(ImGuiKey_GamepadR3, 10);

    ImGui::SFML::SetDPadXAxis(sf::Joystick::Axis::PovX);
    // D-pad Y axis is inverted on Windows
#ifdef _WIN32
    ImGui::SFML::SetDPadYAxis(sf::Joystick::Axis::PovY, true);
#else
    ImGui::SFML::SetDPadYAxis(sf::Joystick::Axis::PovY);
#endif

    ImGui::SFML::SetLStickXAxis(sf::Joystick::Axis::X);
    ImGui::SFML::SetLStickYAxis(sf::Joystick::Axis::Y);
    ImGui::SFML::SetRStickXAxis(sf::Joystick::Axis::U);
    ImGui::SFML::SetRStickYAxis(sf::Joystick::Axis::V);
    ImGui::SFML::SetLTriggerAxis(sf::Joystick::Axis::Z);
    ImGui::SFML::SetRTriggerAxis(sf::Joystick::Axis::R);

    ImGui::SFML::SetJoystickDPadThreshold(5.f);
    ImGui::SFML::SetJoystickLStickThreshold(5.f);
    ImGui::SFML::SetJoystickRStickThreshold(15.f);
    ImGui::SFML::SetJoystickLTriggerThreshold(0.f);
    ImGui::SFML::SetJoystickRTriggerThreshold(0.f);
}

void updateJoystickButtonState(ImGuiIO& io) {
    for (int i = 0; i < static_cast<int>(sf::Joystick::ButtonCount); ++i) {
        const ImGuiKey key = s_currWindowCtx->joystickMapping[i];
        if (key != ImGuiKey_None) {
            const bool isPressed = sf::Joystick::isButtonPressed(s_currWindowCtx->joystickId,
                                                                 static_cast<unsigned>(i));
            if (s_currWindowCtx->windowHasFocus || !isPressed) {
                io.AddKeyEvent(key, isPressed);
            }
        }
    }
}

void updateJoystickAxis(ImGuiIO& io, ImGuiKey key, sf::Joystick::Axis axis, float threshold,
                        float maxThreshold, bool inverted) {
    float pos = sf::Joystick::getAxisPosition(s_currWindowCtx->joystickId, axis);
    if (inverted) {
        pos = -pos;
    }
    const bool passedThreshold = (pos > threshold) == (maxThreshold > threshold);
    if (passedThreshold && s_currWindowCtx->windowHasFocus) {
        io.AddKeyAnalogEvent(key, true, std::abs(pos / 100.f));
    } else {
        io.AddKeyAnalogEvent(key, false, 0);
    }
}

void updateJoystickAxisPair(ImGuiIO& io, ImGuiKey key1, ImGuiKey key2, sf::Joystick::Axis axis,
                            float threshold, bool inverted) {
    updateJoystickAxis(io, key1, axis, -threshold, -100, inverted);
    updateJoystickAxis(io, key2, axis, threshold, 100, inverted);
}

void updateJoystickDPadState(ImGuiIO& io) {
    updateJoystickAxisPair(io, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                           s_currWindowCtx->dPadInfo.xAxis, s_currWindowCtx->dPadInfo.threshold,
                           s_currWindowCtx->dPadInfo.xInverted);
    updateJoystickAxisPair(io, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,
                           s_currWindowCtx->dPadInfo.yAxis, s_currWindowCtx->dPadInfo.threshold,
                           s_currWindowCtx->dPadInfo.yInverted);
}

void updateJoystickAxisState(ImGuiIO& io) {
    updateJoystickAxisPair(io, ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
                           s_currWindowCtx->lStickInfo.xAxis, s_currWindowCtx->lStickInfo.threshold,
                           s_currWindowCtx->lStickInfo.xInverted);
    updateJoystickAxisPair(io, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown,
                           s_currWindowCtx->lStickInfo.yAxis, s_currWindowCtx->lStickInfo.threshold,
                           s_currWindowCtx->lStickInfo.yInverted);

    updateJoystickAxisPair(io, ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight,
                           s_currWindowCtx->rStickInfo.xAxis, s_currWindowCtx->rStickInfo.threshold,
                           s_currWindowCtx->rStickInfo.xInverted);
    updateJoystickAxisPair(io, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown,
                           s_currWindowCtx->rStickInfo.yAxis, s_currWindowCtx->rStickInfo.threshold,
                           s_currWindowCtx->rStickInfo.yInverted);

    updateJoystickAxis(io, ImGuiKey_GamepadL2, s_currWindowCtx->lTriggerInfo.axis,
                       s_currWindowCtx->lTriggerInfo.threshold, 100, false);
    updateJoystickAxis(io, ImGuiKey_GamepadR2, s_currWindowCtx->rTriggerInfo.axis,
                       s_currWindowCtx->rTriggerInfo.threshold, 100, false);
}

void setClipboardText(void* /*userData*/, const char* text) {
    sf::Clipboard::setString(sf::StringUtfUtils::fromUtf8(text, text + std::strlen(text)));
}

const char* getClipboardText(void* /*userData*/) {
    auto tmp = sf::Clipboard::getString().toUtf8();
    s_clipboardText.assign(tmp.begin(), tmp.end());
    return s_clipboardText.c_str();
}

void loadMouseCursor(ImGuiMouseCursor imguiCursorType, sf::Cursor::Type sfmlCursorType) {
    s_currWindowCtx->mouseCursors[imguiCursorType] = sf::Cursor::loadFromSystem(sfmlCursorType);
}

void updateMouseCursor(sf::Window& window) {
    const ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0) {
        const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
        if (io.MouseDrawCursor || cursor == ImGuiMouseCursor_None) {
            window.setMouseCursorVisible(false);
        } else {
            window.setMouseCursorVisible(true);

            const sf::Cursor& c = s_currWindowCtx->mouseCursors[cursor] ?
                                      *s_currWindowCtx->mouseCursors[cursor] :
                                      *s_currWindowCtx->mouseCursors[ImGuiMouseCursor_Arrow];
            window.setMouseCursor(c);
        }
    }
}

ImGuiKey keycodeToImGuiKey(sf::Keyboard::Key code) {
    switch (code) {
    case sf::Keyboard::Key::Tab:
        return ImGuiKey_Tab;
    case sf::Keyboard::Key::Left:
        return ImGuiKey_LeftArrow;
    case sf::Keyboard::Key::Right:
        return ImGuiKey_RightArrow;
    case sf::Keyboard::Key::Up:
        return ImGuiKey_UpArrow;
    case sf::Keyboard::Key::Down:
        return ImGuiKey_DownArrow;
    case sf::Keyboard::Key::PageUp:
        return ImGuiKey_PageUp;
    case sf::Keyboard::Key::PageDown:
        return ImGuiKey_PageDown;
    case sf::Keyboard::Key::Home:
        return ImGuiKey_Home;
    case sf::Keyboard::Key::End:
        return ImGuiKey_End;
    case sf::Keyboard::Key::Insert:
        return ImGuiKey_Insert;
    case sf::Keyboard::Key::Delete:
        return ImGuiKey_Delete;
    case sf::Keyboard::Key::Backspace:
        return ImGuiKey_Backspace;
    case sf::Keyboard::Key::Space:
        return ImGuiKey_Space;
    case sf::Keyboard::Key::Enter:
        return ImGuiKey_Enter;
    case sf::Keyboard::Key::Escape:
        return ImGuiKey_Escape;
    case sf::Keyboard::Key::Apostrophe:
        return ImGuiKey_Apostrophe;
    case sf::Keyboard::Key::Comma:
        return ImGuiKey_Comma;
    case sf::Keyboard::Key::Hyphen:
        return ImGuiKey_Minus;
    case sf::Keyboard::Key::Period:
        return ImGuiKey_Period;
    case sf::Keyboard::Key::Slash:
        return ImGuiKey_Slash;
    case sf::Keyboard::Key::Semicolon:
        return ImGuiKey_Semicolon;
    case sf::Keyboard::Key::Equal:
        return ImGuiKey_Equal;
    case sf::Keyboard::Key::LBracket:
        return ImGuiKey_LeftBracket;
    case sf::Keyboard::Key::Backslash:
        return ImGuiKey_Backslash;
    case sf::Keyboard::Key::RBracket:
        return ImGuiKey_RightBracket;
    case sf::Keyboard::Key::Grave:
        return ImGuiKey_GraveAccent;
    // case : return ImGuiKey_CapsLock;
    // case : return ImGuiKey_ScrollLock;
    // case : return ImGuiKey_NumLock;
    // case : return ImGuiKey_PrintScreen;
    case sf::Keyboard::Key::Pause:
        return ImGuiKey_Pause;
    case sf::Keyboard::Key::Numpad0:
        return ImGuiKey_Keypad0;
    case sf::Keyboard::Key::Numpad1:
        return ImGuiKey_Keypad1;
    case sf::Keyboard::Key::Numpad2:
        return ImGuiKey_Keypad2;
    case sf::Keyboard::Key::Numpad3:
        return ImGuiKey_Keypad3;
    case sf::Keyboard::Key::Numpad4:
        return ImGuiKey_Keypad4;
    case sf::Keyboard::Key::Numpad5:
        return ImGuiKey_Keypad5;
    case sf::Keyboard::Key::Numpad6:
        return ImGuiKey_Keypad6;
    case sf::Keyboard::Key::Numpad7:
        return ImGuiKey_Keypad7;
    case sf::Keyboard::Key::Numpad8:
        return ImGuiKey_Keypad8;
    case sf::Keyboard::Key::Numpad9:
        return ImGuiKey_Keypad9;
    // case : return ImGuiKey_KeypadDecimal;
    case sf::Keyboard::Key::Divide:
        return ImGuiKey_KeypadDivide;
    case sf::Keyboard::Key::Multiply:
        return ImGuiKey_KeypadMultiply;
    case sf::Keyboard::Key::Subtract:
        return ImGuiKey_KeypadSubtract;
    case sf::Keyboard::Key::Add:
        return ImGuiKey_KeypadAdd;
    // case : return ImGuiKey_KeypadEnter;
    // case : return ImGuiKey_KeypadEqual;
    case sf::Keyboard::Key::LControl:
        return ImGuiKey_LeftCtrl;
    case sf::Keyboard::Key::LShift:
        return ImGuiKey_LeftShift;
    case sf::Keyboard::Key::LAlt:
        return ImGuiKey_LeftAlt;
    case sf::Keyboard::Key::LSystem:
        return ImGuiKey_LeftSuper;
    case sf::Keyboard::Key::RControl:
        return ImGuiKey_RightCtrl;
    case sf::Keyboard::Key::RShift:
        return ImGuiKey_RightShift;
    case sf::Keyboard::Key::RAlt:
        return ImGuiKey_RightAlt;
    case sf::Keyboard::Key::RSystem:
        return ImGuiKey_RightSuper;
    case sf::Keyboard::Key::Menu:
        return ImGuiKey_Menu;
    case sf::Keyboard::Key::Num0:
        return ImGuiKey_0;
    case sf::Keyboard::Key::Num1:
        return ImGuiKey_1;
    case sf::Keyboard::Key::Num2:
        return ImGuiKey_2;
    case sf::Keyboard::Key::Num3:
        return ImGuiKey_3;
    case sf::Keyboard::Key::Num4:
        return ImGuiKey_4;
    case sf::Keyboard::Key::Num5:
        return ImGuiKey_5;
    case sf::Keyboard::Key::Num6:
        return ImGuiKey_6;
    case sf::Keyboard::Key::Num7:
        return ImGuiKey_7;
    case sf::Keyboard::Key::Num8:
        return ImGuiKey_8;
    case sf::Keyboard::Key::Num9:
        return ImGuiKey_9;
    case sf::Keyboard::Key::A:
        return ImGuiKey_A;
    case sf::Keyboard::Key::B:
        return ImGuiKey_B;
    case sf::Keyboard::Key::C:
        return ImGuiKey_C;
    case sf::Keyboard::Key::D:
        return ImGuiKey_D;
    case sf::Keyboard::Key::E:
        return ImGuiKey_E;
    case sf::Keyboard::Key::F:
        return ImGuiKey_F;
    case sf::Keyboard::Key::G:
        return ImGuiKey_G;
    case sf::Keyboard::Key::H:
        return ImGuiKey_H;
    case sf::Keyboard::Key::I:
        return ImGuiKey_I;
    case sf::Keyboard::Key::J:
        return ImGuiKey_J;
    case sf::Keyboard::Key::K:
        return ImGuiKey_K;
    case sf::Keyboard::Key::L:
        return ImGuiKey_L;
    case sf::Keyboard::Key::M:
        return ImGuiKey_M;
    case sf::Keyboard::Key::N:
        return ImGuiKey_N;
    case sf::Keyboard::Key::O:
        return ImGuiKey_O;
    case sf::Keyboard::Key::P:
        return ImGuiKey_P;
    case sf::Keyboard::Key::Q:
        return ImGuiKey_Q;
    case sf::Keyboard::Key::R:
        return ImGuiKey_R;
    case sf::Keyboard::Key::S:
        return ImGuiKey_S;
    case sf::Keyboard::Key::T:
        return ImGuiKey_T;
    case sf::Keyboard::Key::U:
        return ImGuiKey_U;
    case sf::Keyboard::Key::V:
        return ImGuiKey_V;
    case sf::Keyboard::Key::W:
        return ImGuiKey_W;
    case sf::Keyboard::Key::X:
        return ImGuiKey_X;
    case sf::Keyboard::Key::Y:
        return ImGuiKey_Y;
    case sf::Keyboard::Key::Z:
        return ImGuiKey_Z;
    case sf::Keyboard::Key::F1:
        return ImGuiKey_F1;
    case sf::Keyboard::Key::F2:
        return ImGuiKey_F2;
    case sf::Keyboard::Key::F3:
        return ImGuiKey_F3;
    case sf::Keyboard::Key::F4:
        return ImGuiKey_F4;
    case sf::Keyboard::Key::F5:
        return ImGuiKey_F5;
    case sf::Keyboard::Key::F6:
        return ImGuiKey_F6;
    case sf::Keyboard::Key::F7:
        return ImGuiKey_F7;
    case sf::Keyboard::Key::F8:
        return ImGuiKey_F8;
    case sf::Keyboard::Key::F9:
        return ImGuiKey_F9;
    case sf::Keyboard::Key::F10:
        return ImGuiKey_F10;
    case sf::Keyboard::Key::F11:
        return ImGuiKey_F11;
    case sf::Keyboard::Key::F12:
        return ImGuiKey_F12;
    default:
        break;
    }
    return ImGuiKey_None;
}

ImGuiKey keycodeToImGuiMod(sf::Keyboard::Key code) {
    switch (code) {
    case sf::Keyboard::Key::LControl:
    case sf::Keyboard::Key::RControl:
        return ImGuiKey_ModCtrl;
    case sf::Keyboard::Key::LShift:
    case sf::Keyboard::Key::RShift:
        return ImGuiKey_ModShift;
    case sf::Keyboard::Key::LAlt:
    case sf::Keyboard::Key::RAlt:
        return ImGuiKey_ModAlt;
    case sf::Keyboard::Key::LSystem:
    case sf::Keyboard::Key::RSystem:
        return ImGuiKey_ModSuper;
    default:
        break;
    }
    return ImGuiKey_None;
}

} // end of anonymous namespace
