#include <windows.h>
#undef GetCurrentTime
#include <MddBootstrap.h>
#include <appmodel.h>
#include <shellapi.h>
#include <commctrl.h>
#include <winrt/Windows.ApplicationModel.h>
#include <optional>
#include <map>
#include <set>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <array>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <vector>
#include "model.hpp"
#include "bridge.hpp"
#include "link.hpp"
#include "updates.hpp"
#include "spectrum.hpp"
#include "wave_brush.hpp"
#include "stage_chain.hpp"
#include "editor_chain.hpp"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;
using Microsoft::UI::Xaml::Controls::Primitives::ToggleButton;
using Windows::Foundation::Point;
using Windows::UI::Color;
namespace fs = std::filesystem;
namespace Shapes = Microsoft::UI::Xaml::Shapes;
namespace Dispatching = Microsoft::UI::Dispatching;
namespace Automation = Microsoft::UI::Xaml::Automation;

static fs::path directory;
// Values from SonyHeadphonesClient's mdr-c/Headphones.h, which only the owner process links.
enum : int {
    MDR_AUDIO_PRIORITY_QUALITY = 1, MDR_AUDIO_PRIORITY_STABILITY = 2,
    MDR_ASSIGNABLE_NONE = 0, MDR_ASSIGNABLE_PLAYBACK = 1, MDR_ASSIGNABLE_NOISE_CONTROL = 2, MDR_ASSIGNABLE_NOISE_CONTROL_QUICK_ACCESS = 3,
    MDR_ASSIGNABLE_TRACK_CONTROL = 4, MDR_ASSIGNABLE_VOICE_RECOGNITION = 5, MDR_ASSIGNABLE_GOOGLE_ASSISTANT = 6, MDR_ASSIGNABLE_AMAZON_ALEXA = 7,
    MDR_ASSIGNABLE_QUICK_ACCESS = 10, MDR_ASSIGNABLE_VOLUME = 11, MDR_ASSIGNABLE_NOISE_CONTROL_MIC = 21,
};
static bool start_in_tray = false; // --tray: opened at sign-in, so it starts hidden in the tray
static bool render_review = false, demo_music = false, log_audio = false, profile_motion=false, profile_silent=false;
struct CostSample{const char* name;double ms;};
static std::vector<CostSample> costs;
struct CostScope{const char* name;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();~CostScope(){if(profile_motion&&costs.size()<200000)costs.push_back({name,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()});}};
static std::wstring review_view = L"Main", review_pass = L"pass1";
static bool review_silent = false; // render-review only: no music, to show Find your EQ paused
static int initial_width = 1280, initial_height = 800;
static fs::path review_timeline;
static double review_time = 30;
static bool review_sequence = false, review_motion = false, review_wave_motion=false, stage_bench=false, readme_shots=false; // readme_shots: the README's screenshots, with demo music (development only). stage_bench: 25 s of the stage with demo music and GPU timers (development only)

static Color rgba(unsigned rgb, double alpha = 1) {
    return Color{static_cast<uint8_t>(std::clamp(alpha, 0.0, 1.0) * 255 + .5), static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
}
static SolidColorBrush brush(unsigned rgb, double alpha = 1) { return SolidColorBrush(rgba(rgb, alpha)); }
static std::wstring signed_value(int value) { return (value > 0 ? L"+" : L"") + std::to_wstring(value); }
static double ease(double t) { return t * t * (3 - 2 * t); }

// The background never changes with the noise mode or EQ. Only the music moves the light.
static constexpr unsigned kBase = 0x000000; // true black: the music glow is the only colour
static constexpr double kLightAlpha = .28;

// One line of the sound field, as an offset from the middle of the stage before scaling. The noise
// mode sets the line's character, and the EQ being heard bends every line into its shape.
// 15 lines (9 until 0.5.11). Spacing is unchanged where the stage has room; line_gap narrows it so
// the outermost line and its sway stay inside the stage's band. Seen from above, like a camera
// focused on the middle: brightness falls away continuously from the centre line, the outer lines
// go out of focus (a wider soft edge) and move a little less, so the stack has depth and no edge.
// (0.5.11-0.5.17 had a flat band of nine full lines and a short ramp, which read as a hard edge.)
static constexpr int kFieldLines = dusk::WaveFrame::kLines;
static constexpr double kLineMiddle = (kFieldLines - 1) / 2.0;
static double line_gap = 1;
// 1 at the centre line, easing down from just off-centre to .14 at the outermost (owner's pick:
// .07 with the wide soft edge left the outer pair all but invisible, .22 was too bright).
static double line_taper(int line) {
    const double d = std::abs(line - kLineMiddle) / kLineMiddle;
    const double t = std::clamp((d - .1) / .9, 0.0, 1.0);
    return 1 - .86 * t * t * (3 - 2 * t);
}
// How out of focus a line is: 0 for the middle third, rising to 1 at the edges.
static double line_blur(int line) {
    const double d = std::abs(line - kLineMiddle) / kLineMiddle;
    const double t = std::clamp((d - .3) / .7, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
static double field_offset(dusk::Noise noise, dusk::Curve const& curve, int line, double x) {
    const double bend = -5.5 * dusk::eq_bend(curve, std::clamp(x / 640, -1.0, 1.0));
    switch (noise) {
    case dusk::Noise::Ambient: return (line - kLineMiddle) * 30 * line_gap + 15 * std::sin(x * .012 + line * .7) + bend;
    case dusk::Noise::Off: return bend;
    default: return (line - kLineMiddle) * 26 * line_gap + 30 * std::pow(std::clamp((std::abs(x) - 300) / 340, 0.0, 1.0), 1.5) * std::sin(x * .028 + line * .7) + bend;
    }
}
static double field_alpha(dusk::Noise noise) { return noise == dusk::Noise::Ambient ? .46 : noise == dusk::Noise::Off ? .05 : .34; }

// A soft glow, with stops along a Gaussian-like falloff so no edge shows.
struct Glow {
    static inline const auto falloff=[] {std::array<std::pair<double,double>,65> result{};for(size_t i=0;i<result.size();++i){double r=double(i)/64;result[i]={r,(std::exp(-5*r*r)-std::exp(-5.0))/(1-std::exp(-5.0))};}return result;}();
    std::vector<GradientStop> stops;
    RadialGradientBrush fill{nullptr};
    unsigned previous_rgb=0;int previous_alpha=-1;
    Shapes::Rectangle rectangle{nullptr};
    void attach(Shapes::Rectangle const& target) {
        fill = RadialGradientBrush();
        for (auto [offset, _] : falloff) {
            GradientStop stop;
            stop.Offset(offset);
            fill.GradientStops().Append(stop);
            stops.push_back(stop);
        }
        target.Fill(fill);
        rectangle = target;
    }
    void color(unsigned rgb, double alpha) {
        const int quantised=static_cast<int>(std::clamp(alpha,0.0,1.0)*255+.5);
        if(rgb==previous_rgb&&quantised==previous_alpha)return;
        previous_rgb=rgb;previous_alpha=quantised;
        for (size_t i = 0; i < stops.size(); ++i) stops[i].Color(rgba(rgb, alpha * falloff[i].second));
        // WinUI 1.7's radial brush can retain its composition gradient when
        // only a stop's Color changes. Notify the stop collection as well;
        // preserve the brush/root instead of replacing the whole background.
        if(!stops.empty())fill.GradientStops().SetAt(0,stops[0]);
    }
};

// A highlight that slides between the buttons of a group, instead of each one just switching.
struct Pill {
    Border border{nullptr};
    TranslateTransform move;
    bool composition=false;double render_width=0;
    double x = 0, width = 0, from_x = 0, from_width = 0, to_x = 0, to_width = 0, t = 1;
    void apply() {
        dusk::Slide position{x,from_x,to_x,t},extent{width,from_width,to_width,t};
        position.step(0);extent.step(0);x=position.value;width=extent.value;
        if(composition)return;
        move.X(x);
        border.Width(std::max(0.0, width));
    }
};

struct LibraryTile { bool composition=false; TranslateTransform motion; Button button{nullptr}; size_t index = 0; double x = 0, y = 0, target_x = 0, target_y = 0; };

struct DuskApp : ApplicationT<DuskApp, Markup::IXamlMetadataProvider> {
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata;
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& name) { return metadata.GetXamlType(name); }
    Markup::IXamlType GetXamlType(hstring const& name) { return metadata.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return metadata.GetXmlnsDefinitions(); }

    Window window{nullptr};
    Grid root{nullptr};
    dusk::Model model;
    dusk::Spectrum spectrum;
    dusk::ReactiveState reactive;
    double last_tick = 0; // milliseconds, from precise_ms()
    double frame_seconds=.016;
    std::array<double, dusk::kBands> pulse{};
    Glow light;
    winrt::com_ptr<WaveBrush> curve_brush; // the EQ editor's curve glow
    StageChain stage; // the main stage's lines and glow
    EditorChain editor_chain; // the EQ editor's spectrum, curve fill and compare afterimage
    std::array<float, dusk::kFine> fine_shown{}; double ghost_t = 0;
    dusk::WaveFrame curve_frame;std::vector<Point> curve_cache;bool curve_glow_stale=true;
    dusk::WaveFrame wave_frame;uint64_t line_sequence=0;
    bool closing=false;
    double glow_level=0;bool music_playing=false;
    bool guide_paused=false;double guide_silence_ms=0; // Find your EQ waits for music (0.7.4)
    std::optional<dusk::Guide> guide;
    // ---------- The owner (EarbudControls.exe): the only process talking to the earbuds ----------
    std::unique_ptr<earbud::OwnerLink> link;
    bool owner_ready = false, owner_library = false, library_imported = false;
    uint32_t generation = 0;
    int request_id = 0;
    earbud::Library sent_library;
    std::optional<earbud::Library> pending_library;
    std::string pushed_key;
    Dispatching::DispatcherQueueTimer push_timer{nullptr}, library_timer{nullptr}, keep_timer{nullptr};
    // Editor: changes save as you go. Undo steps back through whole-model snapshots; a drag of
    // one control is one step. "Reset" returns to the model as it was when the editor opened.
    std::vector<dusk::Model> history; dusk::Model opened; uint64_t last_change = 0; int last_kind = -1;
    bool comparing = false; dusk::Curve compare_curve{}; bool guide_from_home = false;
    std::vector<std::pair<size_t, FontIcon>> library_pencils;
    // The two earbud slot cards. The body is what slides out when a hovering tile would take its place.
    struct SlotCard { Button card{nullptr}; Grid body{nullptr}; };
    int drop_slot = -1; bool drop_allowed = false; std::vector<SlotCard> slot_cards;
    std::vector<FontIcon> bank_pencils;
    std::vector<Border> guide_dots;
    std::vector<dusk::Curve> saved_cache,draft_cache;
    double phase = 0;
    int dither_width=0,dither_height=0;
    bool light_allowed=true;
    CompositeTransform dither_transform;
    Dispatching::DispatcherQueueTimer timer{nullptr};
    double blend = 1;
    struct FieldLine { std::vector<float> from, to, now; double from_alpha{}, to_alpha{}, alpha{}; };
    std::array<FieldLine, kFieldLines> field;
    double field_width = 0, field_scale = 1, field_x = 0, field_y = 0;
    std::array<double, 6> eq_shown{}, eq_from{};
    double eq_blend = 1;
    Shapes::Path eq_core{nullptr};std::vector<Shapes::Ellipse> eq_handles;double plot_width=0,plot_height=0;
    std::vector<ToggleButton> bank_tiles, bank_chips;
    std::vector<Shapes::Path> tile_curves, editor_curves;
    std::vector<TextBlock> editor_names,editor_badges;
    std::vector<FrameworkElement> motion_keys;
    Pill noise_pill, bank_pill, eq_pill, guide_pill;
    bool updating = false, high_contrast = false, animations = true, listening = false, live = false;
    std::wstring page = L"Main";
    std::array<Slider, 5> band_sliders{nullptr, nullptr, nullptr, nullptr, nullptr};
    std::array<TextBlock, 5> band_values{nullptr, nullptr, nullptr, nullptr, nullptr};
    std::vector<ToggleButton> cycle_keys;
    bool drawer_open = false;
    bool drawer_composition=false;
    Control drawer_focus{nullptr};
    double drawer_t = 0;
    TranslateTransform drawer_move;
    std::vector<LibraryTile> library;
    std::vector<size_t> library_order;
    int drag_index = -1;
    bool drag_lifted = false, drag_suppress_click = false;
    Point drag_start{}, drag_offset{};

    template <class T> T get(const wchar_t* name) { return root.FindName(name).as<T>(); }
    void show(const wchar_t* name, bool visible) { get<FrameworkElement>(name).Visibility(visible ? Visibility::Visible : Visibility::Collapsed); }
    Brush theme_brush(const wchar_t* name) {
        return root.Resources().ThemeDictionaries().Lookup(box_value(high_contrast ? L"HighContrast" : L"Default")).as<ResourceDictionary>().Lookup(box_value(name)).as<Brush>();
    }

    void OnLaunched(LaunchActivatedEventArgs const&) {
        UnhandledException([](auto&&, UnhandledExceptionEventArgs const& args) {
            std::ofstream(directory / L"dusk-errors.log", std::ios::app) << "Unhandled: " << to_string(args.Message()) << '\n';
        });
        try {
            Resources().MergedDictionaries().Append(XamlControlsResources());
            for (auto [key, rgb] : std::initializer_list<std::pair<const wchar_t*, unsigned>>{{L"SystemAccentColor", 0xF2F2F2}, {L"SystemAccentColorLight1", 0xFFFFFF}, {L"SystemAccentColorLight2", 0xF2F2F2}, {L"SystemAccentColorLight3", 0xFFFFFF}, {L"SystemAccentColorDark1", 0xD6D6D6}, {L"SystemAccentColorDark2", 0xB8B8B8}, {L"SystemAccentColorDark3", 0x9A9A9A}})
                Resources().Insert(box_value(key), box_value(rgba(rgb)));
            std::wifstream file(directory / L"Dusk.xaml");
            file.imbue(std::locale(".UTF-8"));
            std::wstringstream source;
            source << file.rdbuf();
            root = Markup::XamlReader::Load(hstring(source.str())).as<Grid>();
            Windows::UI::ViewManagement::UISettings ui;
            animations = ui.AnimationsEnabled() && !render_review;
            if (render_review) glow_level = 1; // exports stand in for music playing
            high_contrast = Windows::UI::ViewManagement::AccessibilitySettings().HighContrast();
            noise_pill.border = get<Border>(L"NoisePill");
            bank_pill.border = get<Border>(L"BankPill");
            eq_pill.border = get<Border>(L"EqPill");
            guide_pill.border = get<Border>(L"GuidePill");
            {   // Find your EQ's selected tile glows like the Clear Bass column (owner, 5 Nov 2026):
                // strongest at the bottom, in the music's colour, white and faint in silence.
                guide_glow_brush = LinearGradientBrush(); guide_glow_brush.StartPoint({.5f, 1}); guide_glow_brush.EndPoint({.5f, 0});
                for (auto [offset, alpha] : {std::pair{0.f, .22f}, std::pair{1.f, .06f}}) { GradientStop stop; stop.Offset(offset); stop.Color(Color{static_cast<uint8_t>(255 * alpha), 255, 255, 255}); guide_glow_brush.GradientStops().Append(stop); }
                guide_pill.border.Background(guide_glow_brush);
            }
            for (Pill* pill : {&noise_pill, &bank_pill, &eq_pill, &guide_pill}) pill->border.RenderTransform(pill->move);
            get<UIElement>(L"Drawer").RenderTransform(drawer_move);
            build_haze();
            build_buds();
            build_noise();
            build_eq();
            build_settings();
            bind();
            if (!render_review) load_saved_library();
            rebuild_banks();
            sync_eq();
            navigate(L"Main");
            if (!high_contrast) root.Background(brush(kBase)); // set once: replacing it repaints the whole window
            apply_light();
            root.SizeChanged([this](auto&&, auto&&) { build_dither();layout_banks(); draw_curve(); layout_library(false); });
            get<Grid>(L"Stage").SizeChanged([this](auto&&, auto&&) { draw_field(); });
            get<Canvas>(L"CurveCanvas").SizeChanged([this](auto&&, auto&&) { draw_curve(); });
            for (auto name : {L"NoiseHost", L"BankHost", L"EqBankHost"}) get<FrameworkElement>(name).SizeChanged([this](auto&&, auto&&) { sync_pills(false); });
            get<FrameworkElement>(L"GuideChoiceHost").SizeChanged([this](auto&&, auto&&) { place_guide_pill(false); });
            get<ScrollViewer>(L"LibraryScroll").SizeChanged([this](auto&&,auto&&){if(drawer_open)layout_library(false);});
            get<FrameworkElement>(L"SettingsBody").SizeChanged([this](auto&&, auto&&) { layout_settings(); });
            timer = Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer();
            timer.Interval(std::chrono::milliseconds(33));
            timer.Tick([this](auto&&, auto&&) { tick(); });
            window = Window();
            window.Title(L"Bandwave");
            window.Content(root);
            window.ExtendsContentIntoTitleBar(true);
            window.SetTitleBar(get<FrameworkElement>(L"TitleDrag"));
            auto bar = window.AppWindow().TitleBar();
            bar.PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Tall);
            bar.ButtonForegroundColor(rgba(0xF2F2F2));
            bar.ButtonHoverForegroundColor(rgba(0xFFFFFF));
            bar.ButtonInactiveForegroundColor(rgba(0xF2F2F2, .55));
            bar.ButtonBackgroundColor(rgba(0, 0));
            bar.ButtonInactiveBackgroundColor(rgba(0, 0));
            bar.ButtonHoverBackgroundColor(rgba(0xFFFFFF, .1));
            if (render_review) {
                set_review_state();
                // Screenshots for the README need the whole page at the screen's scale and the handle
                // to capture it with; other review exports keep their 1:1 offscreen window.
                check_hresult(window.as<IWindowNative>()->get_WindowHandle(&main_hwnd));
                const double shot_scale = readme_shots ? GetDpiForWindow(main_hwnd) / 96.0 : 1.0;
                window.AppWindow().MoveAndResize({-20000, -20000, static_cast<int>(initial_width * shot_scale), static_cast<int>(initial_height * shot_scale)});
                root.Width(initial_width);
                root.Height(initial_height);
                RectangleGeometry frame;
                frame.Rect({0, 0, static_cast<float>(initial_width), static_cast<float>(initial_height)});
                root.Clip(frame); // keeps stroke overflow out of the exported bounds
                window.AppWindow().Show(false);
                export_review();
                return;
            }
            // Mica stays, though the black page hides it: without a system backdrop Windows' own
            // compositor (dwm) rose from about 2% to 28% of the GPU with music playing (0.5.6).
            window.SystemBackdrop(MicaBackdrop());
            HWND hwnd{};
            check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
            const double scale = GetDpiForWindow(hwnd) / 96.0;
            MONITORINFO monitor{sizeof(monitor)};
            check_bool(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
            window.AppWindow().ResizeClient({std::min<int>(static_cast<int>(initial_width * scale + .5), monitor.rcWork.right - monitor.rcWork.left - 32),
                                             std::min<int>(static_cast<int>(initial_height * scale + .5), monitor.rcWork.bottom - monitor.rcWork.top - 64)});
            if (auto presenter = window.AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>()) {
                presenter.PreferredMinimumWidth(static_cast<int>(960 * scale + .5));
                presenter.PreferredMinimumHeight(static_cast<int>(620 * scale + .5));
            }
            // Only listen while the window can be seen, and never when animations are turned off.
            window.VisibilityChanged([this](auto&&, WindowVisibilityChangedEventArgs const& args) { listen(args.Visible()); send_interest(args.Visible()); });
            // Hidden in the tray is not minimized: without the IsVisible check, hiding turned the
            // waves and audio capture straight back on (about 9-16% GPU with nothing on screen).
            window.AppWindow().Changed([this](auto const& sender, auto const&) {
                if (auto presenter = sender.Presenter().template try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
                    listen(sender.IsVisible() && presenter.State() != Microsoft::UI::Windowing::OverlappedPresenterState::Minimized);
            });
            // The exe's icon on the window, so the title bar, taskbar preview and Alt+Tab show it.
            if (auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE))) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0)));
            }
            main_hwnd = hwnd;
            SetWindowSubclass(hwnd, tray_events, 1, reinterpret_cast<DWORD_PTR>(this));
            if (earbud::updates::load_settings().tray) add_tray();
            // Closing keeps the app in the tray when that's on; Quit in the tray menu really closes it.
            window.AppWindow().Closing([this](auto const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
                if (quitting || !earbud::updates::load_settings().tray) return;
                args.Cancel(true); to_tray();
            });
            window.Closed([this](auto&&, auto&&) { closing=true;save_profile();remove_tray();timer.Stop();unpace(); spectrum.stop();if(profile_motion){auto folder=directory.parent_path().parent_path()/L"review"/L"live-motion";fs::create_directories(folder);std::ofstream out(folder/L"costs.csv");out<<"operation,milliseconds\n";for(auto const& sample:costs)out<<sample.name<<','<<sample.ms<<'\n';} });
            if (start_in_tray) add_tray(); // opened at sign-in: only the tray icon until it's clicked
            else window.Activate();
            load_profile();
            start_owner_link();
            check_for_update();
            live = true;
            // Fades and scaling can only be attached once the elements are live in a window.
            if (animations && !high_contrast) {
                for (auto const& key : motion_keys) attach_key_motion(key);motion_keys.clear();
                for (auto name : {L"NoiseCancelling",L"NoiseAmbient",L"NoiseOff",L"AllButton",L"CompareButton",L"EditorUndoButton",L"GuideOption0",L"GuideOption1",L"GuideOption2",L"GuideOption3",L"GuideOption4"}) attach_key_motion(get<FrameworkElement>(name));
                for (auto const& dot : guide_dots) { ScalarTransition fade; fade.Duration(std::chrono::milliseconds(200)); Vector3Transition grow; grow.Duration(std::chrono::milliseconds(200)); dot.OpacityTransition(fade); dot.ScaleTransition(grow); }

            }
            listen(!start_in_tray);
        } catch (hresult_error const& e) {
            std::ofstream(directory / L"dusk-errors.log") << "Startup: " << to_string(e.message()) << '\n';
            Exit();
        } catch (std::exception const& e) {
            std::ofstream(directory / L"dusk-errors.log") << "Startup: " << e.what() << '\n';
            Exit();
        }
    }

    // ---------- Tray ----------
    HWND main_hwnd{}; bool quitting = false, in_tray = false;
    static constexpr UINT kTrayMessage = WM_APP + 7;
    NOTIFYICONDATAW tray_data() {
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = main_hwnd; data.uID = 1; data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kTrayMessage; data.uVersion = NOTIFYICON_VERSION_4;
        data.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wcscpy_s(data.szTip, L"Bandwave");
        return data;
    }
    // The icon stays in the notification area for as long as the app runs, while the setting is on.
    void add_tray() { if (!in_tray && main_hwnd) { auto data = tray_data(); in_tray = Shell_NotifyIconW(NIM_ADD, &data); Shell_NotifyIconW(NIM_SETVERSION, &data); } }
    void to_tray() {
        add_tray();
        window.AppWindow().Hide(); // hidden, so it stops listening and lets the earbuds go like a closed window
    }
    void remove_tray() { if (in_tray) { auto data = tray_data(); Shell_NotifyIconW(NIM_DELETE, &data); in_tray = false; } }
    void from_tray() { if (!window) return; window.AppWindow().Show(); window.Activate(); SetForegroundWindow(main_hwnd); }
    void quit() { quitting = true; remove_tray(); window.Close(); }
    static LRESULT CALLBACK tray_events(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR self) {
        auto& app = *reinterpret_cast<DuskApp*>(self);
        // Explorer restarting clears every tray icon; put ours back.
        static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        if (message == taskbar_created && app.in_tray) { app.in_tray = false; app.add_tray(); }
        if (message == kTrayMessage) {
            const UINT event = LOWORD(lparam);
            if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP) app.from_tray();
            else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Open Bandwave");
                AppendMenuW(menu, MF_STRING, 2, L"Quit");
                POINT at{}; GetCursorPos(&at); SetForegroundWindow(hwnd);
                const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (chosen == 1) app.from_tray(); else if (chosen == 2) app.quit();
            }
            return 0;
        }
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    void listen(bool visible) {
        if (visible == listening || !animations || high_contrast) return;
        listening = visible;
        if (visible) { last_tick = precise_ms(); reactive.phase = phase; spectrum.start(); timer.Start(); }
        else { spectrum.stop(); timer.Stop(); unpace(); save_profile(); }
    }
    // The learned music profile (reactive.hpp) and the Wave choice live in app-settings.json.
    double profile_saved_ms = 0; int wave_response = 1;
    void load_profile() {
        if (render_review) return; // reviews use the defaults, so captures don't depend on this PC
        auto s = earbud::updates::load_settings();
        wave_response = s.wave;
        if (s.profile) { reactive.profile.mean = (*s.profile)[0]; reactive.profile.spread = (*s.profile)[1]; }
        profile_saved_ms = precise_ms();
    }
    void save_profile() {
        if (render_review) return;
        auto s = earbud::updates::load_settings();
        s.profile = std::array<std::array<double, 7>, 2>{reactive.profile.mean, reactive.profile.spread};
        earbud::updates::save_settings(s); profile_saved_ms = precise_ms();
    }
    // Calm, Balanced or Lively: how far the lines sway for the same music. Music at its usual level
    // gives a pulse of about .18; Balanced's 1.6 brings that back to the sway 0.7.x gave loud music.
    // The 26-DIP cap in draw_field() still keeps the outer lines inside the stage.
    double wave_gain() const { static constexpr double gains[] = {1.0, 1.6, 2.3}; return gains[std::clamp(wave_response, 0, 2)]; }

    // ---------- Background: the spectrum of light ----------
    void build_haze(){
        show(L"Haze",false);
        if(high_contrast){
            show(L"WaveChain",false);
            for(Pill* pill:{&noise_pill,&bank_pill,&eq_pill})pill->border.Visibility(Visibility::Collapsed);
            for(auto name:{L"NoiseHost",L"BankHost",L"EqBankHost"})get<FrameworkElement>(name).Resources().Clear();
            root.Background(Application::Current().Resources().Lookup(box_value(L"ApplicationPageBackgroundThemeBrush")).as<Brush>());
        }
    }
    void layout_light(){show(L"Haze",false);}
    void build_dither(){CostScope cost{"build_dither"};}
    uint64_t last_noise = 0, last_voice = 0, last_ambient = 0;
    void set_noise(dusk::Noise noise, bool from_device = false) {
        const bool changed = noise != model.noise;
        model.noise = noise;
        sync_noise(true);
        if (!changed) return;
        if (!from_device) { send_command(L"noise", noise == dusk::Noise::Cancelling ? 1 : noise == dusk::Noise::Ambient ? 2 : 0); last_noise = GetTickCount64(); }
        retarget();
    }
    // Sends the sound lines and colours towards the current noise mode and EQ.
    void retarget() {
        if (high_contrast) return;
        blend = animations ? 0 : 1;
        for (int i = 0; i < kFieldLines; ++i) {
            auto& item = field[static_cast<size_t>(i)];
            item.from = item.now;
            item.from_alpha = item.alpha;
            item.to_alpha = field_alpha(model.noise) * line_taper(i);
            for (size_t k = 0; k < item.to.size(); ++k) item.to[k] = static_cast<float>(field_offset(model.noise, model.heard(), i, (static_cast<double>(k) * kFieldStep - dusk::WaveFrame::padding - field_x) / field_scale) * field_scale + field_y);
        }
        if (animations) wake();
        else apply_field(0);
    }
    // The stage is paced by the display's own frames, so each update lands on a refresh: a free
    // timer at 33 ms landed unevenly on a 120 Hz screen (3, then 5 refreshes) and the waves
    // stuttered. It updates on every Nth frame, with N from the monitor's refresh rate so any
    // display gets at least the target rate (60 Hz: every frame; 120 Hz: every 2nd; 144 Hz: 72).
    // Windows redraws only when the waves change, so frames in between cost nothing.
    winrt::event_token rendering_token{}; bool rendering = false; int frame_every = 1, frames_left = 0; double target_fps = 60;
    static double precise_ms() { static const double scale = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return 1000.0 / static_cast<double>(f.QuadPart); }(); LARGE_INTEGER c{}; QueryPerformanceCounter(&c); return static_cast<double>(c.QuadPart) * scale; }
    double refresh_hz() const {
        MONITORINFOEXW monitor{}; monitor.cbSize = sizeof(monitor);
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (main_hwnd && GetMonitorInfoW(MonitorFromWindow(main_hwnd, MONITOR_DEFAULTTONEAREST), &monitor) && EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) return mode.dmDisplayFrequency;
        return 60;
    }
    void pace(double fps) {
        if (!rendering || fps != target_fps) { target_fps = fps; frame_every = std::max(1, static_cast<int>(refresh_hz() / fps)); }
        if (rendering) return;
        rendering = true; frames_left = 0;
        rendering_token = Microsoft::UI::Xaml::Media::CompositionTarget::Rendering([this](auto&&, auto&&) {
            if (frames_left-- > 0) return;
            frames_left = frame_every - 1; tick();
        });
    }
    void unpace() { if (rendering) { Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(rendering_token); rendering = false; } }
    void wake() {
        if (!timer || rendering) return;
        if (!timer.IsRunning()) last_tick = precise_ms();
        timer.Interval(std::chrono::milliseconds(16));
        timer.Start();
    }
    void sync_noise(bool animate) {
        updating = true;
        get<ToggleButton>(L"NoiseCancelling").IsChecked(model.noise == dusk::Noise::Cancelling);
        get<ToggleButton>(L"NoiseAmbient").IsChecked(model.noise == dusk::Noise::Ambient);
        get<ToggleButton>(L"NoiseOff").IsChecked(model.noise == dusk::Noise::Off);
        updating = false;
        place_noise_pill(animate);
    }
    void move_compositor(FrameworkElement const& key,float x,float y,int milliseconds,bool animate){
        Hosting::ElementCompositionPreview::SetIsTranslationEnabled(key,true);
        auto visual=Hosting::ElementCompositionPreview::GetElementVisual(key);
        if(!animate){visual.StopAnimation(L"Translation");visual.Properties().InsertVector3(L"Translation",{x,y,0});return;}
        auto animation=visual.Compositor().CreateVector3KeyFrameAnimation();animation.InsertExpressionKeyFrame(0,L"this.StartingValue");
        auto easing=visual.Compositor().CreateCubicBezierEasingFunction({1.f/3,1},{2.f/3,1});animation.InsertKeyFrame(1,{x,y,0},easing);animation.Duration(std::chrono::milliseconds(milliseconds));visual.StartAnimation(L"Translation",animation);
    }
    void place_pill(Pill& pill, FrameworkElement const& key, FrameworkElement const& host, bool animate) {
        if (high_contrast) return;
        if (!key || key.ActualWidth() <= 0) {
            if (!key) pill.border.Visibility(Visibility::Collapsed);
            return;
        }
        pill.border.Visibility(Visibility::Visible);
        const auto at = key.TransformToVisual(host).TransformPoint(Point{0, 0});
        pill.border.Height(key.ActualHeight());
        const bool first = pill.to_width <= 0;
        pill.from_x = animate && !first ? pill.x : at.X;
        pill.from_width = animate && !first ? pill.width : key.ActualWidth();
        pill.to_x = at.X;
        pill.to_width = key.ActualWidth();
        pill.t = animate && animations && !first ? 0 : 1;
        Canvas::SetTop(pill.border, at.Y);
        if(live){
            if(!pill.composition){pill.border.RenderTransform(nullptr);move_compositor(pill.border,static_cast<float>(pill.x),0,200,false);pill.composition=true;}
            const double old_width=pill.render_width>0?pill.render_width:key.ActualWidth();pill.border.Width(key.ActualWidth());pill.render_width=key.ActualWidth();
            auto visual=Hosting::ElementCompositionPreview::GetElementVisual(pill.border);
            if(animate&&animations&&!first){auto resize=visual.Compositor().CreateScalarKeyFrameAnimation();resize.SetScalarParameter(L"ratio",static_cast<float>(old_width/key.ActualWidth()));resize.InsertExpressionKeyFrame(0,L"this.StartingValue * ratio");resize.InsertKeyFrame(1,1,visual.Compositor().CreateCubicBezierEasingFunction({1.f/3,1},{2.f/3,1}));resize.Duration(std::chrono::milliseconds(200));visual.StartAnimation(L"Scale.X",resize);}else{visual.StopAnimation(L"Scale.X");visual.Scale({1,1,1});}
            move_compositor(pill.border,static_cast<float>(at.X),0,200,animate&&animations&&!first);pill.x=at.X;pill.width=key.ActualWidth();pill.t=1;
        }else pill.apply();
        if (pill.t < 1) wake();
    }
    void place_noise_pill(bool animate) {
        const auto key = model.noise == dusk::Noise::Cancelling ? L"NoiseCancelling" : model.noise == dusk::Noise::Ambient ? L"NoiseAmbient" : L"NoiseOff";
        place_pill(noise_pill, get<FrameworkElement>(key), get<FrameworkElement>(L"NoiseHost"), animate);
    }
    void sync_pills(bool animate) {
        place_noise_pill(animate);
        const bool in_bank = model.selected < bank_tiles.size();
        place_pill(bank_pill, in_bank ? bank_tiles[model.selected].as<FrameworkElement>() : FrameworkElement{nullptr}, get<FrameworkElement>(L"BankHost"), animate);
        place_pill(eq_pill, in_bank ? bank_chips[model.selected].as<FrameworkElement>() : FrameworkElement{nullptr}, get<FrameworkElement>(L"EqBankHost"), animate);
    }

    void tick() {
        CostScope cost{"tick"};
        const double clock = precise_ms();
        const double interval = std::clamp(clock - last_tick, 0.0, 250.0);
        last_tick = clock;frame_seconds=interval/1000;
        const double now = clock / 1000.0;
        if(profile_motion&&costs.size()<200000)costs.push_back({"callback_interval",interval});
        auto levels = spectrum.levels();
        bool playing = spectrum.playing();
        if (demo_music) {
            // Bands rising and falling at different rates, standing in for real music.
            for (size_t i = 0; i < dusk::kBands; ++i)
                levels[i] = static_cast<float>(std::clamp(.5 + .32 * std::sin(now * (.7 + i * .37) + i * 1.3) + .18 * std::pow(std::max(0.0, std::sin(now * 2.7 + i)), 3), 0.0, 1.0));
            playing = true;
        }
        music_playing = playing || (render_review && !review_silent);
        // Find your EQ is chosen by ear: once the music has been quiet a little longer than
        // spectrum's 1.5 s hold (so a gap between tracks doesn't flicker it), the guide pauses,
        // and it carries on as soon as music plays again.
        if (guide) {
            guide_silence_ms = music_playing ? 0 : guide_silence_ms + interval;
            if (const bool paused = guide_silence_ms >= 1000; paused != guide_paused) { guide_paused = paused; apply_guide_pause(); }
        }
        // The editor's analyser: 48 bands, quick to rise and slow to fall, like a level meter.
        auto fine = spectrum.fine();
        if (demo_music) for (size_t i = 0; i < dusk::kFine; ++i) {
            const float f = std::sqrt(dusk::fine_edge(i) * dusk::fine_edge(i + 1)); size_t b = 0;
            while (b + 1 < dusk::kBands && f >= dusk::kBandEdges[b + 1]) ++b;
            fine[i] = levels[b] * static_cast<float>(.7 + .3 * std::sin(now * 3.1 + i * .9));
        }
        float fine_energy = 0;
        for (size_t i = 0; i < dusk::kFine; ++i) {
            // Slow enough for the eye to follow (owner, 5 Nov 2026; was 40 ms up, 220 ms down).
            const double rate = std::min(1.0, interval / (fine[i] > fine_shown[i] ? 120.0 : 500.0));
            fine_shown[i] += static_cast<float>((fine[i] - fine_shown[i]) * rate); fine_energy += fine_shown[i];
        }
        const bool ghost_moving = ghost_t != (comparing ? 1.0 : 0.0);
        if (ghost_moving) ghost_t = std::clamp(ghost_t + (comparing ? 1 : -1) * interval / 150, 0.0, 1.0);
        const bool decor_active=stage_on(); // the stage keeps moving behind the library's glass, and behind Settings while previewing the Wave choice
        // The music's colour moves on the editor too, not just the main page (it froze there until 0.5.21).
        if(decor_active||page==L"Eq")reactive.step(levels, interval / 1000);
        if (playing && clock - profile_saved_ms > 60000) save_profile(); // at most once a minute
        if (wave_preview && !preview_possible()) request_wave_preview(false); // the music stopped or the earbuds went
        pulse = reactive.pulse; phase = reactive.phase;
        const double energy = reactive.energy;
        const bool lit = *std::max_element(pulse.begin(), pulse.end()) > .004;
        if (log_audio) std::ofstream(directory / L"dusk-audio.log", std::ios::app) << (playing ? "playing" : "silent") << " energy=" << energy << " phase=" << phase << '\n';
        const bool morphing = blend < 1, reshaping = eq_blend < 1;
        if (morphing) blend = std::min(1.0, blend + interval / 900);
        const bool glowing = glow_level != (music_playing ? 1.0 : 0.0);
        if (glowing) glow_level = music_playing ? std::min(1.0, glow_level + interval / 500) : std::max(0.0, glow_level - interval / 1200);
        if(stage_on())apply_light();
        if(page==L"Eq")apply_curve_glow();
        if (reshaping) {
            eq_blend = std::min(1.0, eq_blend + interval / 420);
            const double t = ease(eq_blend);
            const auto target = eq_target();
            for (size_t i = 0; i < eq_shown.size(); ++i) eq_shown[i] = eq_from[i] + (target[i] - eq_from[i]) * t;
            updating=true;
            for(size_t i=0;i<5;++i)band_sliders[i].Value(eq_shown[i]);
            updating=false;
            draw_curve();
        }
        bool sliding = false;
        for (Pill* pill : {&noise_pill, &bank_pill, &eq_pill, &guide_pill}) if (pill->t < 1) {
            pill->t = std::min(1.0, pill->t + interval / 200);
            pill->apply();
            sliding = true;
        }
        if (drawer_t != (drawer_open ? 1.0 : 0.0)) {
            drawer_t = std::clamp(drawer_t + (drawer_open ? 1 : -1) * interval / 240, 0.0, 1.0);
            apply_drawer();
        }
        const bool tiles_moving = animate_library();
        const bool stage_visible = stage_on();
        if (stage_visible) apply_field(now);
        const bool busy = blend < 1 || glowing || eq_blend < 1 || sliding || tiles_moving || (drawer_t != (drawer_open ? 1.0 : 0.0));
        if ((!listening || (!controls_live && presence == 0)) && !busy && !lit) { timer.Stop(); unpace(); return; }
        // The stage: 60 updates a second with music or while something moves, 30 for the drift.
        if (stage_visible && !render_review) { timer.Stop(); pace(busy || (decor_active && playing) ? 60 : 30); return; }
        if (page == L"Eq") {
            draw_editor();
            // The analyser moves with the music; in silence, once it has settled, nothing redraws.
            if ((playing || fine_energy > .01f || ghost_moving || busy) && !render_review) { timer.Stop(); pace(60); return; }
        }
        unpace();
        timer.Interval(std::chrono::milliseconds(busy ? 16 : decor_active&&playing ? 33 : stage_visible ? 50 : 250));
        if (!timer.IsRunning()) timer.Start();
    }
    // One light, in one colour, that brightens gently with the sound.
    // kNoColourEasing: the glow's colour is set directly on every frame. Easing it with a
    // compositor animation restarted every 33 ms kept Windows recomposing the whole window at the
    // display's rate (120 Hz here) instead of the app's 30 frames: dwm used about 27% of the GPU
    // for it (0.5.7, 2560x1600). The colour already changes every frame, so nothing is lost.
    void draw_stage(){
        if(closing||high_contrast||!stage_on()||wave_frame.sequence==0||wave_frame.height<=0)return;
        CostScope cost{"draw_stage"};
        if(!stage.panel){if(std::error_code e;fs::exists(earbud::data_folder()/L"gpu-timing.on",e))stage.timers.log=earbud::data_folder()/L"gpu-timing.csv";if(stage_bench){stage.timers.log=directory.parent_path().parent_path()/L"review"/review_pass/L"gpu-timing.csv";std::error_code e;fs::create_directories(stage.timers.log.parent_path(),e);fs::remove(stage.timers.log,e);}stage.bloom_scale=.775f;/* sqrt(9/15): the bloom was tuned for 9 lines, and 15 would wash out the centre */stage.attach(get<Controls::SwapChainPanel>(L"WaveChain"));}
        stage.frost=static_cast<float>(1-std::pow(1-drawer_t,3)); // frosts in step with the library sliding up
        stage.draw(wave_frame);
    }
    void apply_light() {
        CostScope cost{"apply_light"};
        show(L"Haze",false);
        // No glow at all in silence; it fades in with the music and out after it.
        const bool visible=stage_on()&&!high_contrast&&glow_level>0;
        if(!visible){stage.set_colour(rgba(0,0));return;} // the next stage frame draws without glow
        double energy=0;for(auto value:pulse)energy+=value/dusk::kBands;const double level=ease(glow_level);
        stage.set_colour(rgba(dusk::spectrum_color(phase),level*(.24+.10*std::min(1.0,energy*1.4))));
    }

    // ---------- Main screen ----------
    // Rendered by render_earbuds.py, because volume and soft light are what make them read as real.
    Image bud_art(const wchar_t* file) {
        Image art;
        art.Width(370);
        art.Height(340);
        art.Source(Imaging::BitmapImage(Windows::Foundation::Uri(hstring(L"file:///" + (directory / L"Assets" / file).generic_wstring()))));
        return art;
    }
    void build_buds() {
        get<Viewbox>(L"BudLeft").Child(bud_art(L"bud-left.png"));
        get<Viewbox>(L"BudRight").Child(bud_art(L"bud-right.png"));
        get<Viewbox>(L"SettingsBudLeft").Child(bud_art(L"bud-left.png"));
        get<Viewbox>(L"SettingsBudRight").Child(bud_art(L"bud-right.png"));
    }
    void build_noise() {
        auto add = [this](const wchar_t* name, dusk::Noise noise) {
            get<ToggleButton>(name).Click([this, noise](auto&&, auto&&) { set_noise(noise); });
        };
        add(L"NoiseCancelling", dusk::Noise::Cancelling);
        add(L"NoiseAmbient", dusk::Noise::Ambient);
        add(L"NoiseOff", dusk::Noise::Off);
        sync_noise(false);
    }
    // A point every 12 DIPs (8 until 0.5.11): the shortest wave is about 220 DIPs long, so the lines
    // stay smooth, and with 15 lines it is a third less work for the lines and their glow.
    static constexpr int kFieldStep = 12;
    void draw_field() {
        auto stage = get<Grid>(L"Stage");
        const double w = stage.ActualWidth(), h = stage.ActualHeight();
        if (w < 10 || h < 10 || high_contrast) return;
        field_scale = std::min({1.0, (w - 32) / 780, (h - 16) / 280});
        // The outermost line plus its wave and the music's sway (about 45 units) stays 12 DIPs inside.
        // The lines sit in a band that starts 88 DIPs below the stage's top (56 until 0.7.5; the owner
        // wanted the wave lower, twice), so the strip above the library's glass stays clear and the
        // frosted waves never show their low-resolution edge. The bottom keeps its 12 DIPs.
        const double band_top = std::min(88.0, h * .19), band = h - band_top - 12;
        line_gap = std::clamp(((band / 2 - 12) / field_scale - 45) / (kLineMiddle * 30), .6, 1.0);
        RectangleGeometry clip;clip.Rect({0,0,static_cast<float>(w),static_cast<float>(h)});stage.Clip(clip);
        field_width = w;
        field_x = w / 2;
        field_y = band_top + band / 2;
        // The not-connected card shares the stage's cell: centre it on the waves, not the cell.
        get<FrameworkElement>(L"OfflineCard").Margin(ThicknessHelper::FromLengths(24, 24 + 2 * std::max(0.0, field_y - h / 2), 24, 24));
        wave_frame.fade = static_cast<float>(std::min(48.0, h * .1));
        const double s = field_scale;
        const size_t points = static_cast<size_t>((w+2*dusk::WaveFrame::padding) / kFieldStep) + 2;
        for (int i = 0; i < kFieldLines; ++i) {
            auto& item = field[static_cast<size_t>(i)];
            wave_frame.stroke[static_cast<size_t>(i)] = static_cast<float>(1.1 - .3 * line_blur(i));
            wave_frame.soft[static_cast<size_t>(i)] = static_cast<float>(.5 + 5 * line_blur(i));
            item.to.resize(points);
            for (size_t k = 0; k < points; ++k) item.to[k] = static_cast<float>(field_offset(model.noise, model.heard(), i, (static_cast<double>(k) * kFieldStep - dusk::WaveFrame::padding - field_x) / s) * s + field_y);
            item.from = item.now = item.to;
            item.alpha = item.from_alpha = item.to_alpha = field_alpha(model.noise) * line_taper(i);
        }
        blend = 1;
        apply_field(0);
    }
    // The lines contort from one shape to the next, starting at the window's edges and rippling in
    // towards the earbuds. At rest they sway very slightly, and the part of each line under a band
    // of the sound stirs with that band.
    double ambient_mix = 0, presence = 1; // presence: 0 while disconnected, when the lines hold still
    void apply_field(double now) {
        CostScope cost{"apply_field"};
        if (field_width <= 0) return;
        const double center = field_x, s = field_scale;
        int index = 0;
        ambient_mix += ((model.noise == dusk::Noise::Ambient ? 1.0 : 0.0) - ambient_mix) * std::min(1.0, frame_seconds * 3);
        presence += ((controls_live || render_review ? 1.0 : 0.0) - presence) * std::min(1.0, frame_seconds * 2.5);
        if (presence < .002) presence = 0;
        wave_frame.sequence++;wave_frame.width=field_width;wave_frame.height=get<Grid>(L"Stage").ActualHeight();wave_frame.time=now;
        for (auto& item : field) {
            if (item.now.empty()) return;
            wave_frame.weight[static_cast<size_t>(index)] = static_cast<float>(line_taper(index));
            auto& shared=wave_frame.lines[static_cast<size_t>(index)];shared.clear();shared.reserve(item.now.size());
            const double ripple = blend;
            for (size_t k = 0; k < item.now.size(); ++k) {
                const double x = static_cast<double>(k) * kFieldStep - dusk::WaveFrame::padding;
                const double reach = std::min(1.0, std::abs(x - center) / center);
                const double t = ease(std::clamp(ripple * 1.4 - .4 * (1 - reach), 0.0, 1.0));
                item.now[k] = static_cast<float>(item.from[k] + (item.to[k] - item.from[k]) * t);
                // Blend between neighbouring bands, so the line stays smooth where they meet.
                const double position = std::clamp(x / field_width * dusk::kBands - .5, 0.0, dusk::kBands - 1.001);
                const size_t band = static_cast<size_t>(position);
                const double stir = wave_gain() * (pulse[band] + (pulse[band + 1] - pulse[band]) * ease(position - band));
                // A slow drift that never stops, even in silence, plus a faster swell the music drives.
                // Ambient's lines are already waves of about the swell's length, and the two would
                // cancel each other out as the swell travels, flattening the line every few seconds.
                // There the swell is shorter, so it rides on the wave instead of erasing it. The two
                // swells crossfade as the mode changes, so the line never jumps.
                // The outer lines are further away: they sway a little less. (A slower pace for them too
                // put neighbouring lines out of step, and they crossed into a braid.)
                const double depth = 1 - .3 * line_blur(index);
                const double swell = (1 - ambient_mix) * std::sin(x * .011 + now * 2 + index * .8) + ambient_mix * std::sin(x * .023 + now * 2.2 + index * .8);
                const double sway = presence * depth * (3.2 * std::sin(x * .005 + now * .35 + index * .6) + std::min(26.0, .5 + 24 * stir) * swell);
                dusk::WavePoint point{static_cast<float>(x),item.now[k]+static_cast<float>(sway*s)};shared.push_back(point);
            }
            item.alpha = item.from_alpha + (item.to_alpha - item.from_alpha) * ease(blend);
            wave_frame.alpha[static_cast<size_t>(index)] = static_cast<float>(item.alpha);
            ++index;
        }
        line_sequence=wave_frame.sequence;draw_stage();
    }

    // ---------- Curves ----------
    static PathGeometry smooth(std::vector<Point> const& p) {
        PathGeometry geometry;
        PathFigure figure;
        figure.StartPoint(p.front());
        figure.IsClosed(false);
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            const Point a = p[i ? i - 1 : i], b = p[i], c = p[i + 1], d = p[std::min(i + 2, p.size() - 1)];
            BezierSegment segment;
            // Keep the first/last and alternating extreme bands within their
            // actual gains instead of Catmull-Rom overshooting the ±10 guides.
            segment.Point1({b.X + (c.X - a.X) / 6, std::clamp(b.Y + (c.Y - a.Y) / 6,std::min(b.Y,c.Y),std::max(b.Y,c.Y))});
            segment.Point2({c.X - (d.X - b.X) / 6, std::clamp(c.Y - (d.Y - b.Y) / 6,std::min(b.Y,c.Y),std::max(b.Y,c.Y))});
            segment.Point3(c);
            figure.Segments().Append(segment);
        }
        geometry.Figures().Append(figure);
        return geometry;
    }
    static std::vector<Point> curve_points(std::array<double, 6> const& curve, double w, double top, double travel) {
        std::vector<Point> points;
        const auto y = [&](double value) { return static_cast<float>(top + (10 - value) / 20.0 * travel); };
        points.push_back({0, y(curve[0])});
        for (int i = 0; i < 5; ++i) points.push_back({static_cast<float>((i + .5) * w / 5), y(curve[static_cast<size_t>(i)])});
        points.push_back({static_cast<float>(w), y(curve[4])});
        return points;
    }
    // The curve and its handles are drawn together from the same numbers, so they can't drift apart.
    void draw_curve() {
        CostScope cost{"draw_curve"};
        auto canvas = get<Canvas>(L"CurveCanvas");
        if(!guide&&!model.current().curve_known){eq_core=nullptr;apply_curve_glow();canvas.Children().Clear();TextBlock note;note.Text(L"Connect your earbuds to see this preset's curve");note.FontSize(14);Canvas::SetTop(note,40);Canvas::SetLeft(note,32);canvas.Children().Append(note);return;}
        const double w = canvas.ActualWidth(), h = canvas.ActualHeight();
        if (w < 10 || h < 60) return;
        bool rebuild=!eq_core||plot_width!=w||plot_height!=h;
        const dusk::Plot plot{w-40,h};
        if(rebuild){
            canvas.Children().Clear();eq_handles.clear();plot_width=w;plot_height=h;
        for (int gain : {-10, 0, 10}) {
            Shapes::Line rule; rule.X1(32); rule.X2(w - 8); rule.Y1(plot.y(gain)); rule.Y2(plot.y(gain));
            rule.Stroke(brush(0xFFFFFF, gain == 0 ? .16 : .07)); rule.Opacity(high_contrast ? 1 : 0); canvas.Children().Append(rule); // the swap chain draws them otherwise
            TextBlock label; label.Text(gain > 0 ? L"+10" : gain == 0 ? L"0" : L"−10");
            label.FontSize(11); label.Foreground(brush(0xF2F2F2,.45));
            Canvas::SetLeft(label,0); Canvas::SetTop(label,std::clamp(plot.y(gain)-8,0.0,h-16)); canvas.Children().Append(label);
        }

            // The editor's swap chain draws the visible curve (editor_chain.hpp), fading out evenly at
            // both ends. This stroke shows only in high contrast, where the swap chain isn't drawn.
            eq_core=Shapes::Path();eq_core.Stroke(brush(0xF2F2F2));eq_core.StrokeThickness(1.5);
            eq_core.StrokeStartLineCap(PenLineCap::Round);eq_core.StrokeEndLineCap(PenLineCap::Round);
            canvas.Children().Append(eq_core);
            for(int i=0;i<5;++i){Shapes::Ellipse handle;handle.Width(10);handle.Height(10);handle.Fill(brush(0x0A0A0A));handle.Stroke(brush(0xF2F2F2));handle.StrokeThickness(2);canvas.Children().Append(handle);eq_handles.push_back(handle);if(guide)fade(handle,0,0);}
        }
        auto points=curve_points(eq_shown,plot.width,plot.top(),plot.travel());for(auto& point:points)point.X+=32;
        eq_core.Data(smooth(points)); eq_core.Opacity(high_contrast ? 1 : 0);
        curve_cache=points;curve_glow_stale=true;apply_curve_glow();layout_bass();draw_editor();
        for(size_t i=0;i<5;++i){Canvas::SetLeft(eq_handles[i],points[i+1].X-5);Canvas::SetTop(eq_handles[i],points[i+1].Y-5);}
    }
    // The Clear Bass column: a zero line, a glow from 0 to the level, and a pill-shaped knob, on the
    // same scale as the bands. Its colour follows the music like the curve's fill.
    LinearGradientBrush bass_glow_brush{nullptr}, guide_glow_brush{nullptr};
    // Find your EQ hides Clear Bass: the graph then takes the column's space back.
    void bass_column(bool shown) {
        get<FrameworkElement>(L"CurveArea").Margin(ThicknessHelper::FromLengths(shown ? 104 : 28, 16, 28, 0));
        get<FrameworkElement>(L"BandValues").Margin(ThicknessHelper::FromLengths(shown ? 148 : 72, 10, 48, 22)); // the labels keep clear of the window's bottom edge
    }
    void layout_bass() {
        auto track = get<FrameworkElement>(L"BassTrack"); const double h = track.ActualHeight(); if (h < 40) return;
        const dusk::Plot plot{1, h}; const double y0 = plot.y(0), yv = plot.y(eq_shown[5]);
        get<FrameworkElement>(L"BassZero").Margin(ThicknessHelper::FromLengths(14, y0, 14, 0));
        // The glow starts flat at the zero line and is rounded only at the knob's end.
        auto glow = get<Border>(L"BassGlow"); const bool up = yv <= y0;
        glow.Margin(ThicknessHelper::FromLengths(0, up ? yv - 5 : y0, 0, 0)); glow.Height(std::abs(yv - y0) + 5);
        glow.CornerRadius(up ? CornerRadiusHelper::FromRadii(9, 9, 0, 0) : CornerRadiusHelper::FromRadii(0, 0, 9, 9));
        get<FrameworkElement>(L"BassKnob").Margin(ThicknessHelper::FromLengths(0, yv - 5, 0, 0));
        if (bass_glow_brush) { const bool up = eq_shown[5] >= 0; bass_glow_brush.StartPoint({.5f, up ? 0.f : 1.f}); bass_glow_brush.EndPoint({.5f, up ? 1.f : 0.f}); }
    }
    void colour_bass(Color tint, double strength) {
        if (!bass_glow_brush) {
            bass_glow_brush = LinearGradientBrush(); GradientStop strong, faint; strong.Offset(0); faint.Offset(1);
            bass_glow_brush.GradientStops().Append(strong); bass_glow_brush.GradientStops().Append(faint);
            get<Border>(L"BassGlow").Background(bass_glow_brush); layout_bass();
        }
        auto stops = bass_glow_brush.GradientStops();
        auto strong = tint; strong.A = static_cast<uint8_t>(255 * (.22 + .3 * strength)); auto faint = tint; faint.A = static_cast<uint8_t>(255 * .04);
        stops.GetAt(0).Color(strong); stops.GetAt(1).Color(faint);
        if (guide && guide_glow_brush) { // a little fainter at the top than the column, so the tile's number stays readable
            auto top = tint; top.A = static_cast<uint8_t>(255 * (.06 + .04 * strength));
            guide_glow_brush.GradientStops().GetAt(0).Color(strong); guide_glow_brush.GradientStops().GetAt(1).Color(top);
        }
    }
    void draw_editor() {
        if (closing || high_contrast || page != L"Eq" || !root.XamlRoot()) return;
        auto canvas = get<Canvas>(L"CurveCanvas"); const double w = canvas.ActualWidth(), h = canvas.ActualHeight();
        if (w < 10 || h < 60) return;
        CostScope cost{"draw_editor"};
        if (!editor_chain.panel) editor_chain.attach(get<Controls::SwapChainPanel>(L"CurveChain"));
        auto panel = get<FrameworkElement>(L"CurveChain");
        EditorChain::Scene scene; scene.width = static_cast<float>(panel.ActualWidth()); scene.height = static_cast<float>(panel.ActualHeight());
        const float ox = 4, oy = 4; // the canvas sits 12 inside the card, the chain 8
        const dusk::Plot plot{w - 40, h};
        scene.zero_y = static_cast<float>(plot.y(0)) + oy; scene.floor_y = static_cast<float>(plot.y(-10)) + oy; scene.top_y = static_cast<float>(plot.y(10)) + oy;
        scene.left = 32 + ox; scene.right = static_cast<float>(32 + plot.width) + ox;
        // The bands sit at 400 Hz to 16 kHz, each 2.5 times the last: the analyser uses the same scale.
        for (size_t i = 0; i < dusk::kFine; ++i) {
            const double f = std::sqrt(dusk::fine_edge(i) * dusk::fine_edge(i + 1));
            scene.spectrum_x[i] = static_cast<float>(32 + plot.width * (std::log(f / 400) / std::log(2.5) + .5) / 5) + ox;
        }
        // Each band blends a little with its neighbours, so single narrow spikes become swells.
        for (size_t i = 0; i < dusk::kFine; ++i)
            scene.spectrum[i] = .5f * fine_shown[i] + .25f * fine_shown[i ? i - 1 : i] + .25f * fine_shown[std::min(i + 1, dusk::kFine - 1)];
        if (eq_core && curve_cache.size() > 1) { scene.curve = sample_curve(curve_cache); for (auto& p : scene.curve) { p.x += ox; p.y += oy; } }
        if (ghost_t > 0 && model.current().curve_known) {
            std::array<double, 6> was{}; for (size_t i = 0; i < 6; ++i) was[i] = compare_curve[i];
            auto points = curve_points(was, plot.width, plot.top(), plot.travel()); for (auto& p : points) p.X += 32;
            scene.ghost = sample_curve(points); for (auto& p : scene.ghost) { p.x += ox; p.y += oy; } scene.ghost_alpha = static_cast<float>(ease(ghost_t));
        }
        const auto colour = rgba(dusk::spectrum_color(phase)); scene.colour = {colour.R / 255.f, colour.G / 255.f, colour.B / 255.f, 1};
        scene.glow = static_cast<float>(ease(glow_level));
        editor_chain.draw(scene);
        colour_bass(Color{255, static_cast<uint8_t>(255 * (1 + (scene.colour.r - 1) * scene.glow)), static_cast<uint8_t>(255 * (1 + (scene.colour.g - 1) * scene.glow)), static_cast<uint8_t>(255 * (1 + (scene.colour.b - 1) * scene.glow))}, scene.glow);
    }
    // The same curve smooth() draws, sampled densely, so the glow follows the stroke exactly.
    static std::vector<dusk::WavePoint> sample_curve(std::vector<Point> const& p) {
        std::vector<dusk::WavePoint> out{{p.front().X, p.front().Y}};
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            const Point a = p[i ? i - 1 : i], b = p[i], c = p[i + 1], d = p[std::min(i + 2, p.size() - 1)];
            const float x1 = b.X + (c.X - a.X) / 6, y1 = std::clamp(b.Y + (c.Y - a.Y) / 6, std::min(b.Y, c.Y), std::max(b.Y, c.Y));
            const float x2 = c.X - (d.X - b.X) / 6, y2 = std::clamp(c.Y - (d.Y - b.Y) / 6, std::min(b.Y, c.Y), std::max(b.Y, c.Y));
            for (int k = 1; k <= 24; ++k) {
                const float t = k / 24.f, u = 1 - t;
                out.push_back({u*u*u*b.X + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*c.X, u*u*u*b.Y + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*c.Y});
            }
        }
        return out;
    }
    // The editor and Find your EQ curve get the same neon as the Home lines: off in silence,
    // fading with the music, in the current colour. The glow stops where the stroke does.
    void apply_curve_glow() {
        if (!root || !root.XamlRoot()) return;
        auto glow = get<Shapes::Rectangle>(L"CurveGlow");
        // The curve's glow is now drawn by the editor's swap chain (editor_chain.hpp), dithered.
        const bool visible = false;
        glow.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        if (!visible) { curve_glow_stale = true; return; }
        if (!curve_brush) { curve_brush = winrt::make_self<WaveBrush>(); curve_brush->initialise(Hosting::ElementCompositionPreview::GetElementVisual(root).Compositor()); glow.Fill(curve_brush.as<Brush>()); }
        if (curve_glow_stale) {
            CostScope cost{"curve_glow"};
            auto line = sample_curve(curve_cache);
            for (auto& point : line) { point.x += 12; point.y += 12; } // CurveGlow fills CurveArea; the canvas sits 12 inside
            curve_frame.open_ends = true; // the glow ends with the stroke and rounds off around its ends
            curve_frame.sequence++; curve_frame.width = plot_width + 24; curve_frame.height = plot_height + 24; curve_frame.dpi = root.XamlRoot().RasterizationScale();
            curve_frame.lines[0] = line; curve_frame.count = 1; // one shape
            curve_brush->draw(curve_frame); curve_glow_stale = false;
        }
        curve_brush->set_colour(rgba(dusk::spectrum_color(phase), ease(glow_level) * .28), false); // see kNoColourEasing
    }
    static PathGeometry mini_geometry(dusk::Curve const& curve, double amount) {
        const auto y = [&](int value) { return static_cast<float>(17 - value * 1.4 * amount); };
        std::vector<Point> points{{2, y(curve[0])}};
        for (int i = 0; i < 5; ++i) points.push_back({static_cast<float>(13 + i * 23), y(curve[static_cast<size_t>(i)])});
        points.push_back({116, y(curve[4])});
        return smooth(points);
    }
    Shapes::Path mini_curve(dusk::Curve const& curve, double alpha) {
        Shapes::Path line;
        line.Data(mini_geometry(curve, 1));
        line.Stroke(brush(0xF2F2F2));
        line.Opacity(alpha);
        line.StrokeThickness(1.5);
        line.Width(118);
        line.Height(34);
        line.HorizontalAlignment(HorizontalAlignment::Left);
        return line;
    }

    // ---------- Banks ----------
    void layout_banks(){
        if(bank_tiles.empty()||root.ActualWidth()<100)return;
        // Reserve the same action space on both pages, and give the six tiles
        // all remaining room at desktop size. Compact keeps a scrollable bank.
        const double width=std::max(136.0,(root.ActualWidth()-56-92-8-30)/6);
        for(size_t i=0;i<bank_tiles.size();++i){
            bank_tiles[i].Width(width);bank_chips[i].Width(width);
            get<Grid>(L"Bank").ColumnDefinitions().GetAt(static_cast<uint32_t>(i)).Width(GridLengthHelper::FromPixels(width));
            get<Grid>(L"EqBank").ColumnDefinitions().GetAt(static_cast<uint32_t>(i)).Width(GridLengthHelper::FromPixels(width));
        }
    }
    void attach_key_motion(FrameworkElement const& key) {
        if(!live||!animations||high_contrast)return;
        ScalarTransition fade;fade.Duration(std::chrono::milliseconds(120));Vector3Transition press;press.Duration(std::chrono::milliseconds(80));key.OpacityTransition(fade);key.ScaleTransition(press);key.Opacity(.94);
        key.CenterPoint({static_cast<float>(key.ActualWidth()/2),static_cast<float>(key.ActualHeight()/2),0});auto weak=make_weak(key);
        key.SizeChanged([weak](auto&&,auto&&){if(auto element=weak.get())element.CenterPoint({static_cast<float>(element.ActualWidth()/2),static_cast<float>(element.ActualHeight()/2),0});});
        key.PointerEntered([weak](auto&&,auto&&){if(auto element=weak.get())element.Opacity(1);});
        key.PointerExited([weak](auto&&,auto&&){if(auto element=weak.get()){element.Opacity(.94);element.Scale({1,1,1});}});
        key.GotFocus([weak](auto&&,auto&&){if(auto element=weak.get())element.Opacity(1);});
        key.LostFocus([weak](auto&&,auto&&){if(auto element=weak.get())element.Opacity(.94);});
        key.AddHandler(UIElement::PointerPressedEvent(),box_value(Input::PointerEventHandler([weak](auto&&,auto&&){if(auto element=weak.get())element.Scale({.985f,.985f,1});})),true);
        key.AddHandler(UIElement::PointerReleasedEvent(),box_value(Input::PointerEventHandler([weak](auto&&,auto&&){if(auto element=weak.get())element.Scale({1,1,1});})),true);
        key.AddHandler(UIElement::PointerCaptureLostEvent(),box_value(Input::PointerEventHandler([weak](auto&&,auto&&){if(auto element=weak.get())element.Scale({1,1,1});})),true);
    }
    ToggleButton profile_tile(size_t i,bool editor){
        auto const& profile=model.profiles[i]; ToggleButton tile; tile.IsChecked(i==model.selected);
        tile.Width(136);tile.Height(96);tile.HorizontalContentAlignment(HorizontalAlignment::Stretch);tile.VerticalContentAlignment(VerticalAlignment::Stretch);
        tile.Padding(ThicknessHelper::FromLengths(12,10,12,10));tile.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        Automation::AutomationProperties::SetName(tile,profile.name);
        Grid content;content.RowDefinitions().Append(RowDefinition());RowDefinition row;row.Height(GridLengthHelper::Auto());content.RowDefinitions().Append(row);
        auto mini=mini_curve(editor&&i==model.selected?profile.draft:profile.saved,profile.curve_known?(i==model.selected?1:.68):0);
        Viewbox fit;fit.Stretch(Stretch::Uniform);fit.HorizontalAlignment(HorizontalAlignment::Stretch);fit.Child(mini);content.Children().Append(fit);
        TextBlock name;name.Text(profile.name);name.FontSize(13);name.TextTrimming(TextTrimming::CharacterEllipsis);Grid::SetRow(name,1);content.Children().Append(name);tile.Content(content);
        if(!editor&&model.kept_slot(i)>=0)content.Children().Append(earbud_badge(model.kept_slot(i)));
        if(profile.factory)content.Children().Append(tile_caption(L"Sony preset"));
        if(!editor){
            // The chosen EQ shows a pencil: selecting it again opens the editor.
            FontIcon pencil;pencil.Glyph(L"\xE70F");pencil.FontSize(12);pencil.Opacity(.75);pencil.HorizontalAlignment(HorizontalAlignment::Right);pencil.VerticalAlignment(VerticalAlignment::Top);
            pencil.Visibility(i==model.selected?Visibility::Visible:Visibility::Collapsed);content.Children().Append(pencil);bank_pencils.push_back(pencil);
            Automation::AutomationProperties::SetHelpText(tile,L"Select again to edit");
        }
        if(editor){
            TextBlock badge;badge.Text(L"Edited");badge.FontSize(10);badge.Foreground(brush(0xF2F2F2,.7));badge.HorizontalAlignment(HorizontalAlignment::Right);badge.VerticalAlignment(VerticalAlignment::Top);badge.Visibility(Visibility::Collapsed);content.Children().Append(badge);
            editor_curves.push_back(mini);editor_names.push_back(name);editor_badges.push_back(badge);
        }else tile_curves.push_back(mini);
        tile.Click([this,i,editor](auto&&,auto&&){if(!editor&&i==model.selected)open_editor();else select_profile(i);});
        if(live)attach_key_motion(tile);else motion_keys.push_back(tile);return tile;
    }
    void refresh_tiles(){
        const bool fresh=saved_cache.size()!=tile_curves.size();if(fresh){saved_cache.resize(tile_curves.size());draft_cache.resize(tile_curves.size());}
        CostScope cost{"refresh_tiles"};
        for(size_t i=0;i<tile_curves.size();++i){
            bank_tiles[i].IsChecked(i==model.selected);bank_chips[i].IsChecked(i==model.selected);
            if(i<bank_pencils.size())bank_pencils[i].Visibility(i==model.selected?Visibility::Visible:Visibility::Collapsed);
            auto const& saved=model.profiles[i].saved;auto const& draft=i==model.selected?model.profiles[i].draft:saved;
            if(fresh||saved_cache[i]!=saved){tile_curves[i].Data(mini_geometry(saved,1));saved_cache[i]=saved;}
            if(fresh||draft_cache[i]!=draft){editor_curves[i].Data(mini_geometry(draft,1));draft_cache[i]=draft;}
            tile_curves[i].Opacity(model.profiles[i].curve_known?(i==model.selected?1:.68):0);editor_curves[i].Opacity(model.profiles[i].curve_known?(i==model.selected?1:.68):0);
            editor_names[i].Text(model.profiles[i].name);editor_badges[i].Visibility(i==model.selected&&model.dirty()?Visibility::Visible:Visibility::Collapsed);
        }
        bool beyond=model.selected>=bank_tiles.size();auto label=model.library_label();
        get<TextBlock>(L"AllLabel").Text(label);get<TextBlock>(L"EqAllLabel").Text(label);
        for(auto name:{L"AllButton",L"EqAllButton"})get<Button>(name).Background(brush(0xFFFFFF,beyond?.17:.08));
        push_heard();queue_library();
    }
    void rebuild_banks(){
        auto bank=get<Grid>(L"Bank"),editor=get<Grid>(L"EqBank");
        for(auto grid:{bank,editor}){grid.Children().Clear();grid.ColumnDefinitions().Clear();}
        saved_cache.clear();draft_cache.clear();bank_pencils.clear();bank_tiles.clear();bank_chips.clear();tile_curves.clear();editor_curves.clear();editor_names.clear();editor_badges.clear();
        for(size_t i=0;i<std::min(model.profiles.size(),dusk::Model::kBankSize);++i){
            for(auto grid:{bank,editor}){ColumnDefinition column;column.Width(GridLengthHelper::FromPixels(136));grid.ColumnDefinitions().Append(column);}
            auto tile=profile_tile(i,false),chip=profile_tile(i,true);Grid::SetColumn(tile,static_cast<int>(i));Grid::SetColumn(chip,static_cast<int>(i));
            bank.Children().Append(tile);editor.Children().Append(chip);bank_tiles.push_back(tile);bank_chips.push_back(chip);
        }layout_banks();refresh_tiles();
    }
    void select_profile(size_t index){
        model.select(index);for(size_t i=0;i<bank_tiles.size();++i){bank_tiles[i].IsChecked(i==model.selected);bank_chips[i].IsChecked(i==model.selected);}
        refresh_tiles();sync_pills(true);sync_eq(true);retarget();if(!library.empty())layout_library(false);
        for(auto& [slot,pencil]:library_pencils)pencil.Visibility(slot==model.selected?Visibility::Visible:Visibility::Collapsed);
    }

    // ---------- EQ editor ----------
    void build_eq() {
        auto band_grid = get<Grid>(L"BandGrid");
        auto value_grid = get<Grid>(L"BandValues");
        const std::array<const wchar_t*, 5> names{L"400 Hz", L"1 kHz", L"2.5 kHz", L"6.3 kHz", L"16 kHz"}; // as Sony's app labels them
        auto style = root.Resources().Lookup(box_value(L"BandSlider")).as<Style>();
        for (int i = 0; i < 5; ++i) {
            band_grid.ColumnDefinitions().Append(ColumnDefinition());
            value_grid.ColumnDefinitions().Append(ColumnDefinition());
            Slider slider;
            slider.Style(style);
            slider.HorizontalAlignment(HorizontalAlignment::Center);
            slider.VerticalAlignment(VerticalAlignment::Stretch);
            Automation::AutomationProperties::SetName(slider, names[static_cast<size_t>(i)]);
            slider.ValueChanged([this, i](auto&&, Primitives::RangeBaseValueChangedEventArgs const& args) {
                if (updating) return;
                push_history(10 + i);
                auto curve = model.current().saved; curve[static_cast<size_t>(i)] = static_cast<int>(std::lround(args.NewValue()));
                commit_curve(curve); after_change();
            });
            Grid::SetColumn(slider, i);
            band_grid.Children().Append(slider);
            band_sliders[static_cast<size_t>(i)] = slider;

            StackPanel readout;
            readout.Spacing(2);
            readout.HorizontalAlignment(HorizontalAlignment::Center);
            TextBlock value;
            value.FontFamily(FontFamily(L"Segoe UI Variable Display"));
            value.FontWeight(Windows::UI::Text::FontWeights::Light());
            value.FontSize(26);
            value.HorizontalAlignment(HorizontalAlignment::Center);
            band_values[static_cast<size_t>(i)] = value;
            TextBlock label;
            label.Text(names[static_cast<size_t>(i)]);
            label.FontSize(12);
            label.HorizontalAlignment(HorizontalAlignment::Center);
            readout.Children().Append(value);
            readout.Children().Append(label);
            Grid::SetColumn(readout, i);
            value_grid.Children().Append(readout);
        }
        get<FrameworkElement>(L"BassTrack").SizeChanged([this](auto&&, auto&&) { layout_bass(); });
        get<Slider>(L"BassSlider").ValueChanged([this](auto&&, Primitives::RangeBaseValueChangedEventArgs const& args) {
            if (updating) return;
            push_history(15);
            auto curve = model.current().saved; curve[5] = static_cast<int>(std::lround(args.NewValue()));
            commit_curve(curve); after_change();
        });
        try { for (auto name : {L"GuideNextButton"}) get<Button>(name).Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>()); } catch (hresult_error const&) {}
        for (int i = 0; i < dusk::Guide::kSteps; ++i) {
            Border dot; dot.Width(18); dot.Height(6); dot.CornerRadius(CornerRadiusHelper::FromUniformRadius(3)); dot.Background(brush(0xF2F2F2));
            dot.CenterPoint({9, 3, 0});
            get<StackPanel>(L"GuideDots").Children().Append(dot); guide_dots.push_back(dot);
        }
    }
    void sync_eq(bool animate = false) {
        updating = true;
        auto& profile = model.current();
        auto bass = get<Slider>(L"BassSlider");
        for (size_t i = 0; i < 5; ++i) {
            band_sliders[i].IsEnabled(profile.curve_known);
            band_sliders[i].Value(profile.draft[i]);
        }
        bass.IsEnabled(profile.curve_known);
        bass.Value(profile.draft[5]);
        auto name = get<TextBox>(L"ProfileName");
        name.Text(profile.name);
        name.IsReadOnly(profile.factory);
        updating = false;
        // Say plainly what will happen, instead of disabling controls without a reason.
        auto status = get<TextBlock>(L"EditedText");
        status.Text(!profile.curve_known ? L"Sony preset. Its curve is read from the earbuds, so it can't be changed here."
                    : profile.factory ? L"Sony preset. Changing it makes your own copy." : L"");
        if (const int slot = model.kept_slot(model.selected); slot >= 0 && !profile.factory)
            status.Text(L"Kept on your earbuds as Custom " + std::to_wstring(slot + 1) + L". Changes update it there too.");
        status.Visibility(status.Text().empty() ? Visibility::Collapsed : Visibility::Visible);
        sync_state();
        if (animate && animations) {
            eq_from = eq_shown;
            eq_blend = 0;
            updating=true;for(size_t i=0;i<5;++i)band_sliders[i].Value(eq_shown[i]);updating=false;
            wake();
        } else {
            eq_blend = 1;
            for (size_t i = 0; i < eq_shown.size(); ++i) eq_shown[i] = profile.draft[i];
            draw_curve();
        }
    }
    void sync_state() {
        auto const& profile = model.current();
        for (size_t i = 0; i < 5; ++i) band_values[i].Text(profile.curve_known?signed_value(profile.draft[i]):L"—");
        get<TextBlock>(L"BassValue").Text(profile.curve_known?signed_value(profile.draft[5]):L"—");
        get<MenuFlyoutItem>(L"DuplicateAction").IsEnabled(profile.curve_known);
        refresh_tiles();
        get<Button>(L"EditorUndoButton").IsEnabled(!history.empty());
        get<MenuFlyoutItem>(L"ResetAction").IsEnabled(!history.empty());
        get<Button>(L"CompareButton").IsEnabled(profile.curve_known);
        get<MenuFlyoutItem>(L"FlatAction").IsEnabled(profile.curve_known);
        get<MenuFlyoutItem>(L"DeleteAction").IsEnabled(!profile.factory);
    }

    // Returns true when this starts a new undo step. A run of changes to the same control within
    // 0.7 s is one step; kind < 0 always starts one.
    bool push_history(int kind) {
        const auto now = GetTickCount64();
        const bool fresh = kind < 0 || kind != last_kind || now - last_change > 700;
        if (fresh) { history.push_back(model); if (history.size() > 100) history.erase(history.begin()); }
        last_kind = kind; last_change = now;
        return fresh;
    }
    void commit_curve(dusk::Curve const& curve) {
        const auto before = model.profiles.size();
        model.set_live(curve);
        if (model.profiles.size() != before) { // a Sony preset just became the listener's own copy
            rebuild_banks(); sync_pills(false);
            const bool was = updating; updating = true;
            auto name = get<TextBox>(L"ProfileName"); name.Text(model.current().name); name.IsReadOnly(false);
            updating = was;
            auto status = get<TextBlock>(L"EditedText"); status.Text(L"Your own copy. The Sony preset stays as it is."); status.Visibility(Visibility::Visible);
        }
    }
    void after_change() {
        eq_blend = 1;
        for (size_t i = 0; i < eq_shown.size(); ++i) eq_shown[i] = model.current().draft[i];
        const bool was = updating; updating = true;
        for (size_t i = 0; i < 5; ++i) band_sliders[i].Value(eq_shown[i]);
        get<Slider>(L"BassSlider").Value(eq_shown[5]);
        updating = was;
        get<TextBlock>(L"BassValue").Text(signed_value(model.current().draft[5]));
        sync_state(); draw_curve();
    }
    void restore(dusk::Model const& state) {
        model = state; model.editing = true; last_kind = -1;
        rebuild_banks(); sync_pills(false); sync_eq(true);
    }
    void rename_current() {
        if (updating || page != L"Eq" || model.current().factory) return;
        const std::wstring name{get<TextBox>(L"ProfileName").Text()};
        if (name.empty() || name == model.current().name) { get<TextBox>(L"ProfileName").Text(model.current().name); return; }
        push_history(-4); model.current().name = name; refresh_tiles(); sync_state();
    }
    void start_compare() {
        if (comparing || guide || !model.current().curve_known) return;
        comparing = true; get<TextBlock>(L"CompareLabel").Text(L"Playing how it was"); push_heard(); wake();
    }
    void end_compare() {
        if (!comparing) return;
        comparing = false; get<TextBlock>(L"CompareLabel").Text(L"Hold to compare"); push_heard(); wake();
    }
    void leave_editor() {
        end_compare();
        if (guide) { guide.reset(); guide_from_home = false; restore_editor(false); }
        model.editing = false; note(L""); rebuild_banks(); navigate(L"Main"); retarget();
    }

    // ---------- Settings, arranged around the earbuds ----------
    FrameworkElement add_row(StackPanel const& panel, std::wstring const& title, std::wstring const& hint, FrameworkElement const& control) {
        Grid row;
        row.MinHeight(48);
        row.ColumnSpacing(14);
        row.BorderThickness(ThicknessHelper::FromLengths(0, 1, 0, 0));
        row.BorderBrush(theme_brush(L"RuleBrush"));
        row.ColumnDefinitions().Append(ColumnDefinition());
        ColumnDefinition end;
        end.Width(GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(end);
        TextBlock label;
        label.Text(title);
        label.FontSize(14);
        label.TextWrapping(TextWrapping::Wrap);
        label.VerticalAlignment(VerticalAlignment::Center);
        row.Children().Append(label);
        ToolTipService::SetToolTip(label, box_value(hint));
        control.VerticalAlignment(VerticalAlignment::Center);
        Grid::SetColumn(control, 1);
        row.Children().Append(control);
        panel.Children().Append(row);
        return row;
    }
    ToggleSwitch make_switch(std::wstring const& name, bool on) {
        ToggleSwitch control;
        control.IsOn(on);
        control.OnContent(nullptr);
        control.OffContent(nullptr);
        control.MinWidth(0);
        Automation::AutomationProperties::SetName(control, name);
        return control;
    }
    FrameworkElement make_choice(std::wstring const& name, std::vector<std::wstring> const& options, size_t selected, std::function<void(size_t)> chosen = {}) {
        Border frame;
        frame.Padding(ThicknessHelper::FromUniformLength(2));
        frame.CornerRadius(CornerRadiusHelper::FromUniformRadius(7));
        frame.Background(brush(0xFFFFFF, high_contrast ? 0 : .07));
        StackPanel keys;
        keys.Orientation(Orientation::Horizontal);
        keys.Spacing(2);
        auto group = std::make_shared<std::vector<ToggleButton>>();
        for (size_t i = 0; i < options.size(); ++i) {
            ToggleButton key;
            key.Content(box_value(options[i]));
            key.MinHeight(40);
            key.FontSize(13);
            key.IsChecked(i == selected);
            Automation::AutomationProperties::SetName(key, name + L": " + options[i]);
            key.Click([group, i, chosen](auto&&, auto&&) { for (size_t k = 0; k < group->size(); ++k) (*group)[k].IsChecked(k == i); if (chosen) chosen(i); });
            group->push_back(key);
            keys.Children().Append(key);
        }
        frame.Child(keys);
        return frame;
    }
    FrameworkElement make_slider(std::wstring const& name, int minimum, int maximum, int value) {
        StackPanel holder;
        holder.Orientation(Orientation::Horizontal);
        holder.Spacing(10);
        Slider slider;
        slider.Minimum(minimum);
        slider.Maximum(maximum);
        slider.StepFrequency(1);
        slider.Value(value);
        slider.Width(150);
        slider.IsThumbToolTipEnabled(false);
        slider.VerticalAlignment(VerticalAlignment::Center);
        Automation::AutomationProperties::SetName(slider, name);
        TextBlock readout;
        readout.Text(std::to_wstring(value));
        readout.Width(20);
        readout.VerticalAlignment(VerticalAlignment::Center);
        slider.ValueChanged([readout](auto&&, Primitives::RangeBaseValueChangedEventArgs const& args) { readout.Text(std::to_wstring(std::lround(args.NewValue()))); });
        holder.Children().Append(slider);
        holder.Children().Append(readout);
        return holder;
    }
    TextBlock make_value(std::wstring const& text) {
        TextBlock value;
        value.Text(text);
        value.FontSize(14);
        return value;
    }
    Button make_action(std::wstring const& text) {
        Button button;
        button.Content(box_value(text));
        button.MinHeight(40);
        button.Padding(ThicknessHelper::FromLengths(14, 0, 14, 0));
        return button;
    }
    StackPanel add_group(StackPanel const& column, std::wstring const& title) {
        StackPanel group;
        TextBlock heading;
        heading.Text(title);
        heading.FontFamily(FontFamily(L"Segoe UI Variable Display"));
        heading.FontWeight(Windows::UI::Text::FontWeights::Light());
        heading.FontSize(24);
        heading.Margin(ThicknessHelper::FromLengths(0, 0, 0, 8));
        group.Children().Append(heading);
        column.Children().Append(group);
        return group;
    }
    static std::wstring app_version() {
        UINT32 length = 0;
        if (GetCurrentPackageFullName(&length, nullptr) == APPMODEL_ERROR_NO_PACKAGE) return L"Development build";
        auto v = Windows::ApplicationModel::Package::Current().Id().Version();
        return std::to_wstring(v.Major) + L"." + std::to_wstring(v.Minor) + L"." + std::to_wstring(v.Build);
    }
    // Installed from the Microsoft Store: the Store keeps the app up to date, so its own update
    // check and Check now stay out of the way. Builds from GitHub keep them.
    static bool store_install() {
        UINT32 length = 0;
        if (GetCurrentPackageFullName(&length, nullptr) == APPMODEL_ERROR_NO_PACKAGE) return false;
        try { return Windows::ApplicationModel::Package::Current().SignatureKind() == Windows::ApplicationModel::PackageSignatureKind::Store; }
        catch (...) { return false; }
    }
    void attach_flyout(const wchar_t* button, std::function<void(StackPanel const&)> fill) {
        Flyout flyout;
        StackPanel panel;
        panel.Width(420);
        fill(panel);
        flyout.Content(panel);
        flyout.Closed([this](auto&&, auto&&) { request_wave_preview(false); }); // the Wave preview never outlives its row
        get<Button>(button).Flyout(flyout);
    }
    std::array<StackPanel, 4> setting_groups{nullptr, nullptr, nullptr, nullptr};
    Slider ambient_slider{nullptr}; ToggleSwitch voice_switch{nullptr}; TextBlock about_firmware{nullptr};
    // A row per earbud setting. It is shown only while the earbuds advertise it, and shows what they
    // last reported; a change is sent at once and the next report confirms or corrects it.
    struct LiveSetting { FrameworkElement row{nullptr}; std::function<void(int)> show_value; size_t group{}; };
    std::map<std::string, LiveSetting> live_settings;
    std::map<std::string, std::vector<int>> touch_options;
    std::map<std::string, Border> touch_hosts;
    StackPanel general_group{nullptr};
    TextBlock connected_devices{nullptr}; ToggleSwitch phone_switch{nullptr};
    void send_setting(std::string const& key, int value) {
        if (!link || !owner_ready) return;
        JsonObject m;
        m.SetNamedValue(L"type", JsonValue::CreateStringValue(L"command"));
        m.SetNamedValue(L"id", JsonValue::CreateNumberValue(++request_id));
        m.SetNamedValue(L"generation", JsonValue::CreateNumberValue(generation));
        m.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(device_address)));
        m.SetNamedValue(L"op", JsonValue::CreateStringValue(L"setting"));
        m.SetNamedValue(L"key", JsonValue::CreateStringValue(to_hstring(key)));
        m.SetNamedValue(L"value", JsonValue::CreateNumberValue(value));
        send_json(m);
        last_setting = GetTickCount64();
    }
    uint64_t last_setting = 0;
    // Segmented keys whose selection can also be set from the earbuds' report.
    std::pair<Border, std::function<void(int)>> make_keys(std::wstring const& name, std::vector<std::pair<std::wstring, int>> const& options, std::function<void(int)> picked) {
        Border frame;
        frame.Padding(ThicknessHelper::FromUniformLength(2));
        frame.CornerRadius(CornerRadiusHelper::FromUniformRadius(7));
        frame.Background(brush(0xFFFFFF, high_contrast ? 0 : .07));
        StackPanel keys; keys.Orientation(Orientation::Horizontal); keys.Spacing(2);
        auto group = std::make_shared<std::vector<std::pair<ToggleButton, int>>>();
        auto select = [group](int value) { for (auto& [key, v] : *group) key.IsChecked(v == value); };
        for (auto const& [label, value] : options) {
            ToggleButton key; key.Content(box_value(label)); key.MinHeight(40); key.FontSize(13);
            Automation::AutomationProperties::SetName(key, name + L": " + label);
            key.Click([this, select, picked, value](auto&&, auto&&) { select(value); if (!updating) picked(value); });
            group->push_back({key, value});
            keys.Children().Append(key);
        }
        frame.Child(keys);
        return {frame, select};
    }
    // General settings are named by the earbuds with Sony's own keys (MULTIPOINT_SETTING, ...), which
    // Sony's app translates. Known keys get plain words; any other reads as a sentence, not a key.
    static std::pair<std::wstring, std::wstring> general_setting_text(std::wstring const& subject) {
        if (subject == L"MULTIPOINT_SETTING") return {L"Connect to two devices at once", L"Stay connected to this PC and your phone without switching. LDAC sound isn't available while both are connected."};
        if (subject == L"SIDETONE_SETTING") return {L"Hear your voice on calls", L"Your own voice comes through the earbuds during calls. Turn it off if it's too loud or brings in background noise."};
        if (subject.find_first_of(L"abcdefghijklmnopqrstuvwxyz ") != std::wstring::npos) return {subject, L""};
        std::wstring words = subject;
        for (auto suffix : {L"_SETTING", L"_SETTINGS"}) { const std::wstring s{suffix}; if (words.size() > s.size() && words.compare(words.size() - s.size(), s.size(), s) == 0) words.resize(words.size() - s.size()); }
        for (size_t i = 0; i < words.size(); ++i) words[i] = words[i] == L'_' ? L' ' : (i ? static_cast<wchar_t>(towlower(words[i])) : words[i]);
        return {words, L""};
    }
    // Makes a settings switch ask before sending: it springs back, and a flyout offers the change.
    std::map<std::string, ToggleSwitch> confirmed_switches; std::set<std::string> confirming;
    void confirm_switch(std::string const& key, std::wstring const& warning) {
        auto it = confirmed_switches.find(key); if (it == confirmed_switches.end()) return;
        auto control = it->second;
        Flyout confirm; StackPanel panel; panel.Spacing(10); panel.Width(280);
        TextBlock ask; ask.Text(warning); ask.TextWrapping(TextWrapping::Wrap);
        Button yes; yes.Content(box_value(L"Change it"));
        panel.Children().Append(ask); panel.Children().Append(yes); confirm.Content(panel);
        yes.Click([this, confirm, control, key](auto&&, auto&&) {
            confirm.Hide(); updating = true; control.IsOn(!control.IsOn()); updating = false; send_setting(key, control.IsOn() ? 1 : 0);
        });
        confirming.insert(key);
        control.Toggled([this, control, confirm, key](auto&&, auto&&) {
            if (updating) return;
            updating = true; control.IsOn(!control.IsOn()); updating = false; // back to what the earbuds have until confirmed
            confirm.ShowAt(control);
        });
    }
    void live_switch(StackPanel const& group, size_t index, std::string const& key, std::wstring const& title, std::wstring const& hint) {
        auto control = make_switch(title, false); confirmed_switches.insert_or_assign(key, control);
        control.Toggled([this, key, control](auto&&, auto&&) { if (!updating && !confirming.count(key)) send_setting(key, control.IsOn() ? 1 : 0); });
        live_settings[key] = {add_row(group, title, hint, control), [control](int v) { control.IsOn(v == 1); }, index};
    }
    static std::wstring touch_label(int action) {
        switch (action) {
        case MDR_ASSIGNABLE_NONE: return L"None";
        case MDR_ASSIGNABLE_PLAYBACK: return L"Playback";
        case MDR_ASSIGNABLE_NOISE_CONTROL: case MDR_ASSIGNABLE_NOISE_CONTROL_QUICK_ACCESS: case MDR_ASSIGNABLE_NOISE_CONTROL_MIC: return L"Noise";
        case MDR_ASSIGNABLE_TRACK_CONTROL: return L"Tracks";
        case MDR_ASSIGNABLE_VOLUME: return L"Volume";
        case MDR_ASSIGNABLE_VOICE_RECOGNITION: case MDR_ASSIGNABLE_GOOGLE_ASSISTANT: case MDR_ASSIGNABLE_AMAZON_ALEXA: return L"Assistant";
        case MDR_ASSIGNABLE_QUICK_ACCESS: return L"Quick access";
        default: return L"";
        }
    }
    // The noise cycle as Sony stores it: which two or three modes a tap moves through.
    static int cycle_mode(std::array<bool, 3> const& c) { return c[0] && c[1] && c[2] ? 1 : c[0] && c[1] ? 2 : c[0] && c[2] ? 3 : 4; }
    static std::array<bool, 3> cycle_modes(int mode) { return mode == 1 ? std::array{true, true, true} : mode == 2 ? std::array{true, true, false} : mode == 3 ? std::array{true, false, true} : std::array{false, true, true}; }
    void build_settings() {
        auto left = get<StackPanel>(L"SettingsLeft");
        auto right = get<StackPanel>(L"SettingsRight");

        auto sound = add_group(left, L"Sound");
        {
            auto [keys, select] = make_keys(L"Bluetooth priority", {{L"Sound", static_cast<int>(MDR_AUDIO_PRIORITY_QUALITY)}, {L"Stable", static_cast<int>(MDR_AUDIO_PRIORITY_STABILITY)}}, [this](int v) { send_setting("priority", v); });
            live_settings["priority"] = {add_row(sound, L"Bluetooth priority", L"Better sound or a steadier connection. The earbuds may reconnect to apply it.", keys), select, 0};
        }
        live_switch(sound, 0, "dsee", L"DSEE", L"Restores detail in compressed music. Applies to every EQ.");
        auto ambient_row = make_slider(L"Ambient level", 1, 20, 14);
        ambient_slider = ambient_row.as<StackPanel>().Children().GetAt(0).as<Slider>();
        ambient_slider.ValueChanged([this](auto&&, Primitives::RangeBaseValueChangedEventArgs const& args) { if (!updating) { send_command(L"ambient", static_cast<int>(std::lround(args.NewValue()))); last_ambient = GetTickCount64(); } });
        add_row(sound, L"Ambient level", L"How much outside sound comes through in Ambient.", ambient_row);
        voice_switch = make_switch(L"Voice focus", false);
        voice_switch.Toggled([this](auto&&, auto&&) { if (!updating) { send_command(L"voice", voice_switch.IsOn() ? 1 : 0); last_voice = GetTickCount64(); } });
        add_row(sound, L"Voice focus", L"In Ambient, lets voices through more than other sounds.", voice_switch);

        auto voice = add_group(left, L"Voice and sounds");
        general_group = voice;
        live_switch(voice, 1, "guidance", L"Voice guidance", L"Spoken prompts for battery and connection.");
        {
            auto row = make_slider(L"Guidance volume", 1, 5, 3);
            auto slider = row.as<StackPanel>().Children().GetAt(0).as<Slider>();
            slider.ValueChanged([this](auto&&, Primitives::RangeBaseValueChangedEventArgs const& args) { if (!updating) send_setting("guidance_volume", static_cast<int>(std::lround(args.NewValue())) - 3); });
            live_settings["guidance_volume"] = {add_row(voice, L"Guidance volume", L"How loud the prompts are.", row), [slider](int v) { slider.Value(v + 3); }, 1};
        }

        auto touch = add_group(right, L"Touch controls");
        for (auto [key, title, side] : {std::tuple{"touch_left", L"Left earbud", L"left"}, std::tuple{"touch_right", L"Right earbud", L"right"}}) {
            Border host; touch_hosts[key] = host;
            live_settings[key] = {add_row(touch, title, std::wstring(L"What a tap on the ") + side + L" earbud controls.", host), {}, 2};
        }
        Border cycle_frame;
        cycle_frame.Padding(ThicknessHelper::FromUniformLength(2));
        cycle_frame.CornerRadius(CornerRadiusHelper::FromUniformRadius(7));
        cycle_frame.Background(brush(0xFFFFFF, high_contrast ? 0 : .07));
        StackPanel cycle_keys_panel;
        cycle_keys_panel.Orientation(Orientation::Horizontal);
        cycle_keys_panel.Spacing(2);
        const std::array<const wchar_t*, 3> modes{L"NC", L"Ambient", L"Off"};
        const std::array<const wchar_t*, 3> full{L"Noise cancelling", L"Ambient", L"Off"};
        for (size_t i = 0; i < modes.size(); ++i) {
            ToggleButton key;
            key.Content(box_value(modes[i]));
            key.MinHeight(40);
            key.FontSize(13);
            key.IsChecked(model.cycle[i]);
            Automation::AutomationProperties::SetName(key, std::wstring(L"Include ") + full[i] + L" in the cycle");
            key.Click([this, i](auto&&, auto&&) {
                const bool changed = model.toggle_cycle(i);
                for (size_t k = 0; k < cycle_keys.size(); ++k) cycle_keys[k].IsChecked(model.cycle[k]);
                if (changed) send_setting("cycle", cycle_mode(model.cycle));
            });
            cycle_keys.push_back(key);
            cycle_keys_panel.Children().Append(key);
        }
        cycle_frame.Child(cycle_keys_panel);
        live_settings["cycle"] = {add_row(touch, L"Noise cycle", L"Modes the touch control moves through. Keep at least two.", cycle_frame), [this](int v) {
            model.cycle = cycle_modes(v); for (size_t k = 0; k < cycle_keys.size(); ++k) cycle_keys[k].IsChecked(model.cycle[k]); }, 2};

        auto wearing = add_group(right, L"Wearing and power");
        live_switch(wearing, 3, "auto_pause", L"Pause when taken out", L"Music pauses when you remove an earbud.");
        live_switch(wearing, 3, "wear_power", L"Power off when not worn", L"Saves battery when the earbuds are left out.");
        {
            auto off = make_action(L"Power off");
            Flyout confirm; StackPanel panel; panel.Spacing(10); panel.Width(260);
            TextBlock ask; ask.Text(L"Turn the earbuds off? They disconnect from this PC until you turn them on again."); ask.TextWrapping(TextWrapping::Wrap);
            Button yes; yes.Content(box_value(L"Turn off"));
            yes.Click([this, confirm](auto&&, auto&&) { confirm.Hide(); send_command(L"power_off", 0); });
            panel.Children().Append(ask); panel.Children().Append(yes); confirm.Content(panel); off.Flyout(confirm);
            live_settings["shutdown"] = {add_row(wearing, L"Turn earbuds off", L"Turns the earbuds off now. They disconnect from this PC.", off), {}, 3};
        }

        setting_groups = {sound, voice, touch, wearing};
        right.Children().Clear();
        right.Children().Append(wearing);
        right.Children().Append(touch);
        if (!render_review) { for (auto& [key, setting] : live_settings) setting.row.Visibility(Visibility::Collapsed); sync_setting_groups(); }

        attach_flyout(L"ConnectionsButton", [this](StackPanel const& panel) {
            connected_devices = make_value(L"—"); connected_devices.TextWrapping(TextWrapping::Wrap); connected_devices.MaxWidth(200);
            add_row(panel, L"Connected devices", L"What the earbuds are connected to right now.", connected_devices);
            phone_switch = make_switch(L"Let the phone app connect", false);
            phone_switch.Toggled([this](auto&&, auto&&) { if (!updating) send_command(L"release_when_hidden", phone_switch.IsOn() ? 1 : 0); });
            add_row(panel, L"Let the phone app connect", L"This app steps aside 15 seconds after its window and widget close, so Sony's phone app can connect.", phone_switch);
        });
        build_app_flyouts();
    }
    // A group with nothing the earbuds offer is hidden, along with its line to the earbud.
    // Disconnected, no group can be changed, so all are hidden and SettingsOffline says why. (Sound used to
    // stay because Ambient level and Voice focus aren't in the earbuds' settings report.)
    bool settings_ready = render_review;
    void sync_setting_groups() {
        std::array<bool, 4> any{settings_ready, false, false, false}; // connected, Sound always has Ambient level and Voice focus
        for (auto& [key, setting] : live_settings) if (settings_ready && setting.row.Visibility() == Visibility::Visible) any[setting.group] = true;
        show(L"SettingsOffline", !settings_ready);
        for (size_t i = 0; i < 4; ++i) setting_groups[i].Visibility(any[i] ? Visibility::Visible : Visibility::Collapsed);
        if (page == L"Settings") { root.UpdateLayout(); layout_settings(); }
    }
    // Shows, hides and fills each row from the earbuds' report.
    void sync_settings(JsonObject const& m, bool ready) {
        if (settings_ready != ready) { settings_ready = ready; if (!setting_groups.empty() && setting_groups[0]) sync_setting_groups(); }
        if (!m.HasKey(L"settings") || m.GetNamedValue(L"settings").ValueType() != JsonValueType::Object) return;
        auto values = m.GetNamedObject(L"settings");
        const bool settling = GetTickCount64() - last_setting < 1500;
        // Touch keys are built from the actions these earbuds offer for each side.
        if (m.HasKey(L"setting_options") && m.GetNamedValue(L"setting_options").ValueType() == JsonValueType::Object) {
            auto options = m.GetNamedObject(L"setting_options");
            for (auto& [key, host] : touch_hosts) {
                std::vector<int> offered;
                if (options.HasKey(to_hstring(key))) { auto list = options.GetNamedArray(to_hstring(key)); for (uint32_t i = 0; i < list.Size(); ++i) offered.push_back(static_cast<int>(list.GetNumberAt(i))); }
                std::erase_if(offered, [](int a) { return touch_label(a).empty(); });
                if (offered == touch_options[key]) continue;
                touch_options[key] = offered;
                std::vector<std::pair<std::wstring, int>> labels; for (int a : offered) labels.push_back({touch_label(a), a});
                auto [keys, select] = make_keys(key == "touch_left" ? L"Left earbud" : L"Right earbud", labels, [this, key](int v) { send_setting(key, v); });
                host.Child(keys); live_settings[key].show_value = select;
            }
        }
        // General settings are named by the earbuds themselves, so their rows are made as they appear.
        if (m.HasKey(L"setting_names") && m.GetNamedValue(L"setting_names").ValueType() == JsonValueType::Object) {
            auto names = m.GetNamedObject(L"setting_names");
            for (auto const& pair : names) {
                const auto key = to_string(pair.Key());
                if (live_settings.count(key) || pair.Value().ValueType() != JsonValueType::String) continue;
                const std::wstring subject{pair.Value().GetString()};
                const auto [title, hint] = general_setting_text(subject);
                live_switch(general_group, 1, key, title, hint);
                // Changing multipoint makes the earbuds drop every connection and reconnect (Sony's
                // protocol names the disconnect after it), so the switch asks first.
                if (subject == L"MULTIPOINT_SETTING") confirm_switch(key, L"Your earbuds will disconnect from all your devices and reconnect. Your phone may need reconnecting from its Bluetooth settings.");
            }
        }
        updating = true;
        for (auto& [key, setting] : live_settings) {
            const auto name = to_hstring(key);
            bool offered = ready && values.HasKey(name) && values.GetNamedValue(name).ValueType() == JsonValueType::Number;
            if (key.starts_with("touch")) offered = offered && touch_options[key].size() >= 2;
            setting.row.Visibility(offered ? Visibility::Visible : Visibility::Collapsed);
            if (offered && !settling && setting.show_value) setting.show_value(static_cast<int>(values.GetNamedNumber(name)));
        }
        if (phone_switch) phone_switch.IsOn(m.GetNamedBoolean(L"release_when_hidden", false));
        updating = false;
        if (connected_devices) {
            std::wstring text; auto devices = m.GetNamedArray(L"devices", JsonArray());
            for (uint32_t i = 0; i < devices.Size(); ++i) text += (text.empty() ? L"" : L", ") + std::wstring(devices.GetStringAt(i));
            connected_devices.Text(text.empty() ? L"—" : text);
        }
        sync_setting_groups();
    }
    // Start with Windows: the package's startup task (AppxManifest.xml). Windows keeps the setting; the
    // user can also turn it off in Settings > Apps > Startup, and then only they can turn it back on.
    bool startup_updating = false;
    winrt::fire_and_forget sync_startup(ToggleSwitch control, std::optional<bool> want) {
        winrt::apartment_context ui;
        using winrt::Windows::ApplicationModel::StartupTask; using winrt::Windows::ApplicationModel::StartupTaskState;
        StartupTaskState state = StartupTaskState::Disabled; bool known = false;
        try {
            auto task = co_await StartupTask::GetAsync(L"SoundConnectStartup");
            state = task.State();
            if (want) { if (*want) state = co_await task.RequestEnableAsync(); else { task.Disable(); state = task.State(); } }
            known = true;
        } catch (...) {}
        co_await ui;
        startup_updating = true;
        control.IsOn(known && (state == StartupTaskState::Enabled || state == StartupTaskState::EnabledByPolicy));
        control.IsEnabled(known && state != StartupTaskState::DisabledByPolicy && state != StartupTaskState::EnabledByPolicy);
        startup_updating = false;
    }
    void build_app_flyouts() {
        attach_flyout(L"AppButton", [this](StackPanel const& panel) {
            // Wave (0.8.0) first (0.8.2): the panel opens upward from its button, so the pointer reaches
            // the other rows before it passes over this one. Resting on it shows the main screen's waves.
            auto wave = make_choice(L"Wave", {L"Calm", L"Balanced", L"Lively"}, static_cast<size_t>(render_review ? 1 : earbud::updates::load_settings().wave),
                [this](size_t chosen) { wave_response = static_cast<int>(chosen); auto s = earbud::updates::load_settings(); s.wave = wave_response; earbud::updates::save_settings(s); wake(); });
            auto over = std::make_shared<bool>(false);
            wave.PointerEntered([this, over](auto&&, auto&&) { *over = true; request_wave_preview(true); });
            wave.PointerExited([this, over](auto&&, auto&&) { *over = false; request_wave_preview(false); });
            wave.PointerCanceled([this, over](auto&&, auto&&) { *over = false; request_wave_preview(false); });
            wave.GotFocus([this](auto&&, RoutedEventArgs const& args) { if (auto c = args.OriginalSource().try_as<Control>(); c && c.FocusState() == FocusState::Keyboard) request_wave_preview(true, true); });
            wave.LostFocus([this, over](auto&&, auto&&) { if (!*over) request_wave_preview(false); });
            add_row(panel, L"Wave", L"How much the lines on the main screen move with your music. They also learn what you usually play, so any kind of music moves them.", wave);
            add_row(panel, L"Widget", L"Shows your six main-screen EQs. Add it from Win + W.", make_value(L"WF-C710N"));
            if (!store_install()) {
            auto updates = make_switch(L"Check for updates", earbud::updates::load_settings().check);
            updates.Toggled([updates](auto&&, auto&&) { auto s = earbud::updates::load_settings(); s.check = updates.IsOn(); earbud::updates::save_settings(s); });
            // Check now (0.8.0): looks straight away, whatever the daily switch says, and answers in its own label.
            auto check_now = make_action(L"Check now");
            Automation::AutomationProperties::SetName(check_now, L"Check for an update now");
            check_now.Click([this, check_now](auto&&, auto&&) { check_for_update(true, check_now); });
            StackPanel update_controls; update_controls.Orientation(Orientation::Horizontal); update_controls.Spacing(12);
            update_controls.Children().Append(check_now); update_controls.Children().Append(updates);
            add_row(panel, L"Check for updates", L"Once a day, from this project's GitHub releases, or now with Check now. Nothing about you is sent.", update_controls);
            }
            auto tray = make_switch(L"Keep running in the tray", earbud::updates::load_settings().tray);
            tray.Toggled([this, tray](auto&&, auto&&) { auto s = earbud::updates::load_settings(); s.tray = tray.IsOn(); earbud::updates::save_settings(s); if (s.tray) add_tray(); else remove_tray(); });
            add_row(panel, L"Keep running in the tray", L"Closing the window keeps Bandwave in the notification area. Quit from its menu there.", tray);
            auto startup = make_switch(L"Start with Windows", false); startup.IsEnabled(false);
            startup.Toggled([this, startup](auto&&, auto&&) { if (!startup_updating) sync_startup(startup, startup.IsOn()); });
            add_row(panel, L"Start with Windows", L"Opens in the tray when you sign in, so the widget and your earbuds are ready. If it won't stay on, allow Bandwave in Windows Settings > Apps > Startup.", startup);
            sync_startup(startup, std::nullopt);
        });
        attach_flyout(L"AboutButton", [this](StackPanel const& panel) {
            add_row(panel, L"Model", L"Your earbuds.", make_value(L"WF-C710N"));
            about_firmware = make_value(L"—");
            add_row(panel, L"Firmware", L"Read from your earbuds.", about_firmware);
            add_row(panel, L"App version", L"Bandwave.", make_value(app_version()));
            auto bluetooth = make_action(L"Open");
            bluetooth.Click([](auto&&, auto&&) { ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL); });
            add_row(panel, L"Bluetooth settings", L"Pair or remove devices in Windows.", bluetooth);
        });
    }
    // Each group is wired to the part of the earbud it concerns. Narrow windows drop the middle.
    void layout_settings() {
        auto body = get<FrameworkElement>(L"SettingsBody");
        const double w = body.ActualWidth();
        if (w <= 0 || page != L"Settings") return;
        // The earbud art fits the space between the columns (0.8.3). It was a fixed 2 x 200 DIPs, which
        // ran under both columns between about 1060 and 1300 DIPs wide. The columns first narrow
        // from 370 to 330, then the art shrinks; below 110 DIPs an earbud is too small to read, and
        // the page drops the art and its lines for two plain columns.
        constexpr double kArtSpacing = 30, kBudMax = 200, kBudMin = 110, kColumnMax = 370, kColumnMin = 330;
        const double column = std::clamp((w - (2 * kBudMax + kArtSpacing)) / 2, kColumnMin, kColumnMax);
        const double bud = std::min(kBudMax, (w - 2 * column - kArtSpacing) / 2);
        const bool wide = bud >= kBudMin;
        const auto star = GridLengthHelper::FromValueAndType(1, GridUnitType::Star);
        get<ColumnDefinition>(L"SettingsLeftColumn").Width(wide ? GridLengthHelper::FromPixels(column) : star);
        get<ColumnDefinition>(L"SettingsRightColumn").Width(wide ? GridLengthHelper::FromPixels(column) : star);
        get<ColumnDefinition>(L"SettingsMiddleColumn").Width(wide ? star : GridLengthHelper::FromPixels(32));
        show(L"SettingsBuds", wide);
        if (wide) for (auto name : {L"SettingsBudLeft", L"SettingsBudRight"}) { auto art = get<FrameworkElement>(name); art.Width(bud); art.Height(bud * .92); }
        if (wide) get<FrameworkElement>(L"SettingsBuds").UpdateLayout(); // the lines below are drawn from the art's new size
        auto lines = get<Canvas>(L"SettingsLines");
        lines.Children().Clear();
        if (!wide || high_contrast) return;
        auto canvas_ui = lines.as<UIElement>();
        auto left_bud = get<FrameworkElement>(L"SettingsBudLeft");
        auto right_bud = get<FrameworkElement>(L"SettingsBudRight");
        struct Wire { size_t group; FrameworkElement bud; double fx, fy; bool from_right; };
        const std::array<Wire, 4> wires{{{0, left_bud, .84, .60, true}, {1, left_bud, .51, .83, true}, {2, right_bud, .39, .45, false}, {3, right_bud, .22, .10, false}}};
        for (auto const& wire : wires) {
            auto group = setting_groups[wire.group];
            // A hidden group keeps its last size, so check that it is shown: on disconnecting, the lines
            // to the hidden groups stayed, pointing at nothing (0.6.0).
            if (group.Visibility() != Visibility::Visible || group.ActualWidth() <= 0 || wire.bud.ActualWidth() <= 0) continue;
            const auto group_at = group.TransformToVisual(canvas_ui).TransformPoint(Point{wire.from_right ? static_cast<float>(group.ActualWidth()) : 0.f, 17});
            const auto bud_at = wire.bud.TransformToVisual(canvas_ui).TransformPoint(Point{static_cast<float>(wire.bud.ActualWidth() * wire.fx), static_cast<float>(wire.bud.ActualHeight() * wire.fy)});
            PathGeometry geometry;
            PathFigure figure;
            figure.StartPoint(group_at);
            figure.IsClosed(false);
            BezierSegment segment;
            const float middle = (group_at.X + bud_at.X) / 2;
            segment.Point1({middle, group_at.Y});
            segment.Point2({middle, bud_at.Y});
            segment.Point3(bud_at);
            figure.Segments().Append(segment);
            geometry.Figures().Append(figure);
            Shapes::Path line;
            line.Data(geometry);
            line.Stroke(brush(0xF2F2F2, .32));
            line.StrokeThickness(1);
            lines.Children().Append(line);
            Shapes::Ellipse dot;
            dot.Width(6);
            dot.Height(6);
            dot.Fill(brush(0xF2F2F2, .8));
            Canvas::SetLeft(dot, bud_at.X - 3);
            Canvas::SetTop(dot, bud_at.Y - 3);
            lines.Children().Append(dot);
        }
    }

    // ---------- EQ library drawer ----------
    static constexpr double kTileHeight = 96, kTileGap = 8, kCrateTop = 26, kRestLabel = 34;
    // Height of one labelled group: its label, the well and the gap before the next label.
    static constexpr double kGroup = kCrateTop + kTileHeight + kTileGap * 2 + 8;
    int library_columns(){return std::clamp(static_cast<int>((root.ActualWidth()-56+8)/144),4,8);}
    double tile_width() {
        const double w = std::max(600.0, root.ActualWidth() - 68);
        return std::clamp((w - kTileGap * (library_columns()-1)) / library_columns(),136.0,176.0);
    }
    Point slot_position(size_t slot) {
        const double width = tile_width();
        if (slot < dusk::Model::kBankSize) return {static_cast<float>(6+(slot%library_columns()) * (width + kTileGap)), static_cast<float>(kGroup+kCrateTop+(slot/library_columns())*(kTileHeight+kTileGap))};
        const size_t rest = slot - dusk::Model::kBankSize;
        const double top = kGroup + kCrateTop + kTileHeight + kTileGap * 2 + kRestLabel;
        return {static_cast<float>(6+(rest % library_columns()) * (width + kTileGap)), static_cast<float>(top + (rest / library_columns()) * (kTileHeight + kTileGap))};
    }
    bool searching() { return !get<TextBox>(L"LibrarySearch").Text().empty(); }
    void rebuild_library() {
        auto canvas = get<Canvas>(L"LibraryCanvas");
        canvas.Children().Clear();
        library.clear();
        library_order.clear();
        library_pencils.clear();
        std::wstring query{get<TextBox>(L"LibrarySearch").Text()};
        std::transform(query.begin(), query.end(), query.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        for (size_t i = 0; i < model.profiles.size(); ++i) {
            std::wstring name = model.profiles[i].name;
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (name.find(query) == std::wstring::npos) continue;
            library_order.push_back(library.size());
            LibraryTile tile;
            tile.index = i;
            tile.button = make_library_tile(library.size(), i);
            if(live){Hosting::ElementCompositionPreview::SetIsTranslationEnabled(tile.button,true);tile.composition=true;}else tile.button.RenderTransform(tile.motion);
            canvas.Children().Append(tile.button);
            library.push_back(tile);
        }
        layout_library(false);
    }
    Button make_library_tile(size_t position, size_t index) {
        auto const& profile = model.profiles[index];
        Button tile;
        tile.Height(kTileHeight);
        tile.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        tile.VerticalContentAlignment(VerticalAlignment::Stretch);
        tile.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
        tile.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        Automation::AutomationProperties::SetName(tile, profile.name);
        Grid content;
        content.RowDefinitions().Append(RowDefinition());
        RowDefinition label_row;
        label_row.Height(GridLengthHelper::Auto());
        content.RowDefinitions().Append(label_row);
        if(profile.curve_known){Viewbox fit;fit.Stretch(Stretch::Uniform);fit.Child(mini_curve(profile.saved,.85));content.Children().Append(fit);}
        else {TextBlock marker;marker.Text(L"Sony preset");marker.FontSize(11);marker.Opacity(.6);content.Children().Append(marker);}
        if(const int slot=model.kept_slot(index);slot>=0)content.Children().Append(earbud_badge(slot));
        if(profile.factory&&profile.curve_known)content.Children().Append(tile_caption(L"Sony preset")); // an unknown curve's marker already says it
        TextBlock name;
        name.Text(profile.name);
        name.FontSize(13);
        name.TextTrimming(TextTrimming::CharacterEllipsis);
        Grid::SetRow(name, 1);
        content.Children().Append(name);
        FontIcon pencil; pencil.Glyph(L"\xE70F"); pencil.FontSize(12); pencil.Opacity(.75); pencil.HorizontalAlignment(HorizontalAlignment::Right); pencil.VerticalAlignment(VerticalAlignment::Top);
        pencil.Visibility(index == model.selected ? Visibility::Visible : Visibility::Collapsed); content.Children().Append(pencil); library_pencils.emplace_back(index, pencil);
        Automation::AutomationProperties::SetHelpText(tile, L"Select again to edit");
        tile.Content(content);
        tile.Click([this, index](auto&&, auto&&) {
            if (drag_suppress_click) { drag_suppress_click = false; return; }
            if (index == model.selected) { close_drawer(); open_editor(); return; } // the chosen EQ opens for editing, as on Home
            select_profile(index);
        });
        tile.RightTapped([this, index](IInspectable const& sender, auto&&) { show_tile_menu(sender.as<FrameworkElement>(), index); });
        if(const int slot=model.kept_slot(index);slot>=0)Automation::AutomationProperties::SetItemStatus(tile,L"On earbuds, Custom "+std::to_wstring(slot+1));
        tile.AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler([this, position](IInspectable const&, Input::PointerRoutedEventArgs const& args) { begin_drag(position, args); })), true);
        tile.AddHandler(UIElement::PointerMovedEvent(), box_value(Input::PointerEventHandler([this](IInspectable const&, Input::PointerRoutedEventArgs const& args) { continue_drag(args); })), true);
        tile.AddHandler(UIElement::PointerReleasedEvent(), box_value(Input::PointerEventHandler([this](IInspectable const&, Input::PointerRoutedEventArgs const&) { end_drag(); })), true);
        tile.AddHandler(UIElement::PointerCaptureLostEvent(), box_value(Input::PointerEventHandler([this](IInspectable const&, Input::PointerRoutedEventArgs const&) { end_drag(); })), true);
        return tile;
    }
    void show_tile_menu(FrameworkElement const& anchor, size_t index) {
        MenuFlyout menu;
        const auto add = [&](const wchar_t* label, std::function<void()> action) {
            MenuFlyoutItem item;
            item.Text(label);
            item.Click([action](auto&&, auto&&) { action(); });
            menu.Items().Append(item);
        };
        add(L"Edit", [this, index] { model.select(index); close_drawer(); open_editor(); });
        add(L"Duplicate", [this, index] { model.select(index); model.duplicate(); library_changed(); });
        if (!model.profiles[index].factory && model.profiles[index].curve_known) {
            MenuFlyoutSubItem keep; keep.Text(L"Keep on earbuds");
            for (size_t k = 0; k < 2; ++k) {
                MenuFlyoutItem item; auto const held = model.find(model.kept[k]);
                item.Text(L"Custom " + std::to_wstring(k + 1) + (held ? L" (replaces " + model.profiles[*held].name + L")" : L""));
                item.IsEnabled(model.kept_slot(index) != static_cast<int>(k));
                item.Click([this, k, index](auto&&, auto&&) { model.keep(k, index); library_changed(); });
                keep.Items().Append(item);
            }
            menu.Items().Append(keep);
            if (const int slot = model.kept_slot(index); slot >= 0) add(L"Remove from earbuds", [this, slot] { model.release(static_cast<size_t>(slot)); library_changed(); });
        }
        if (index >= dusk::Model::kBankSize) add(L"Move to main screen", [this, index] { model.move(index, dusk::Model::kBankSize - 1); library_changed(); });
        else add(L"Move off main screen", [this, index] { model.move(index, dusk::Model::kBankSize); library_changed(); });
        if (index > 0) add(L"Move earlier", [this, index] { model.move(index, index - 1); library_changed(); });
        if (index + 1 < model.profiles.size()) add(L"Move later", [this, index] { model.move(index, index + 1); library_changed(); });
        if (!model.profiles[index].factory) add(L"Delete", [this, index] { model.select(index); model.erase(); library_changed(); });
        menu.ShowAt(anchor);
    }
    void library_changed() {
        rebuild_banks();
        sync_pills(false);
        sync_eq();
        rebuild_library();
        retarget();
    }
    void layout_library(bool animate) {
        CostScope cost{"layout_library"};
        if (library.empty() && !drawer_open) return;
        auto canvas = get<Canvas>(L"LibraryCanvas");
        const double width = tile_width();
        const bool filtered = searching();
        size_t last = 0;
        for (size_t slot = 0; slot < library_order.size(); ++slot) {
            auto& tile = library[library_order[slot]];
            const auto at = filtered ? Point{static_cast<float>(6+(slot % library_columns()) * (width + kTileGap)), static_cast<float>((slot / library_columns()) * (kTileHeight + kTileGap))} : slot_position(slot);
            tile.target_x = at.X;
            tile.target_y = at.Y;
            tile.button.Width(width);
            if(tile.composition){move_compositor(tile.button,at.X,at.Y,200,animate&&animations);tile.x=at.X;tile.y=at.Y;}
            if (tile.index == model.selected) tile.button.Background(brush(0xFFFFFF, .2));
            else tile.button.Background(brush(0xFFFFFF, .07));
            if (!animate || !animations) {
                tile.x = at.X;
                tile.y = at.Y;
                tile.motion.X(tile.x);
                tile.motion.Y(tile.y);
            }
            last = slot;
        }
        (void)last;
        // The crate labels and the well behind the main-screen row.
        auto children = canvas.Children();
        for (int32_t i = static_cast<int32_t>(children.Size()) - 1; i >= 0; --i) {
            auto element = children.GetAt(i).try_as<FrameworkElement>();
            if (!element.try_as<Button>() || unbox_value_or<hstring>(element.Tag(), L"") == L"slot") children.RemoveAt(i);
        }
        slot_cards.clear();
        double height = 0;
        if (!filtered) {
            // On your earbuds: Custom 1 and Custom 2. Manual is never shown; it plays whatever is chosen.
            Border earbuds;
            earbuds.Width(width * 2 + kTileGap + 12); earbuds.Height(kTileHeight + 12);
            earbuds.CornerRadius(CornerRadiusHelper::FromUniformRadius(10)); earbuds.Background(brush(0xFFFFFF, high_contrast ? 0 : .05));
            Canvas::SetTop(earbuds, kCrateTop - 6); children.InsertAt(0, earbuds);
            TextBlock earbuds_label; earbuds_label.Text(L"On your earbuds");
            earbuds_label.FontSize(13); children.InsertAt(0, earbuds_label);
            for (size_t k = 0; k < 2; ++k) { auto slot = earbud_slot(k, width); children.Append(slot.card); slot_cards.push_back(slot); }
            Border well;
            well.Width(width * std::min(6,library_columns()) + kTileGap * (std::min(6,library_columns())-1)+12);
            well.Height(kTileHeight + 12);
            well.CornerRadius(CornerRadiusHelper::FromUniformRadius(10));
            well.Background(brush(0xFFFFFF, high_contrast ? 0 : .05));
            Canvas::SetLeft(well, 0);
            Canvas::SetTop(well, kGroup + kCrateTop - 6);
            children.InsertAt(0, well);
            TextBlock on_screen;
            on_screen.Text(L"Main screen");
            on_screen.FontSize(13);
            Canvas::SetTop(on_screen, kGroup);
            children.InsertAt(0, on_screen);
            TextBlock rest;
            rest.Text(L"Everything else");
            rest.FontSize(13);
            Canvas::SetTop(rest, kGroup * 2);
            children.InsertAt(0, rest);
            const auto end = slot_position(std::max(library_order.size(), dusk::Model::kBankSize + 1) - 1);
            height = end.Y + kTileHeight + 12;
        } else {
            height = ((library_order.size() + library_columns()-1) / library_columns()) * (kTileHeight + kTileGap);
        }
        canvas.Height(height);
    }
    TextBlock earbud_badge(int slot) { return tile_caption(L"On earbuds · Custom " + std::to_wstring(slot + 1)); }
    // Sony presets are marked wherever they appear, so it's clear before opening one that editing
    // makes your own copy (owner, 5 Nov 2026). Same place and style as the earbud slot caption.
    static TextBlock tile_caption(std::wstring const& text) {
        TextBlock badge; badge.Text(text); badge.FontSize(10); badge.Opacity(.7);
        badge.HorizontalAlignment(HorizontalAlignment::Left); badge.VerticalAlignment(VerticalAlignment::Top);
        return badge;
    }
    SlotCard earbud_slot(size_t k, double width) {
        Button card; card.Tag(box_value(L"slot")); card.Width(width); card.Height(kTileHeight);
        card.HorizontalContentAlignment(HorizontalAlignment::Stretch); card.VerticalContentAlignment(VerticalAlignment::Stretch);
        card.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10)); card.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        Canvas::SetLeft(card, 6 + k * (width + kTileGap)); Canvas::SetTop(card, kCrateTop);
        Grid content; content.RowDefinitions().Append(RowDefinition()); RowDefinition row; row.Height(GridLengthHelper::Auto()); content.RowDefinitions().Append(row);
        // The slot's contents can slide out of view, so the card clips them.
        RectangleGeometry clip; clip.Rect({0, 0, static_cast<float>(width - 24), static_cast<float>(kTileHeight - 20)}); content.Clip(clip);
        TextBlock slot_name; slot_name.Text(L"Custom " + std::to_wstring(k + 1)); slot_name.FontSize(10); slot_name.Opacity(.7); content.Children().Append(slot_name);
        Grid body; body.RowDefinitions().Append(RowDefinition()); RowDefinition body_row; body_row.Height(GridLengthHelper::Auto()); body.RowDefinitions().Append(body_row);
        Grid::SetRowSpan(body, 2); content.Children().Append(body);
        const auto held = model.find(model.kept[k]);
        const std::wstring slot_label = L"Custom " + std::to_wstring(k + 1) + L" on your earbuds";
        if (held) {
            auto const& profile = model.profiles[*held];
            Viewbox fit; fit.Stretch(Stretch::Uniform); fit.Child(mini_curve(profile.saved, .85)); body.Children().Append(fit);
            TextBlock name; name.Text(profile.name); name.FontSize(13); name.TextTrimming(TextTrimming::CharacterEllipsis); Grid::SetRow(name, 1); body.Children().Append(name);
            Automation::AutomationProperties::SetName(card, slot_label + L": " + profile.name);
            const size_t index = *held;
            card.Click([this, index](auto&&, auto&&) { if (index == model.selected) { close_drawer(); open_editor(); } else select_profile(index); });
            card.RightTapped([this, k](IInspectable const& sender, auto&&) {
                MenuFlyout menu; MenuFlyoutItem item; item.Text(L"Remove from earbuds");
                item.Click([this, k](auto&&, auto&&) { model.release(k); library_changed(); }); menu.Items().Append(item); menu.ShowAt(sender.as<FrameworkElement>());
            });
        } else {
            TextBlock hint; hint.Text(L"Drag an EQ here"); hint.FontSize(13); hint.Opacity(.6); hint.HorizontalAlignment(HorizontalAlignment::Center); hint.VerticalAlignment(VerticalAlignment::Center);
            Grid::SetRowSpan(hint, 2); body.Children().Append(hint);
            Automation::AutomationProperties::SetName(card, slot_label + L", empty");
            Automation::AutomationProperties::SetHelpText(card, L"Drag an EQ here, or right-click an EQ and choose Keep on earbuds");
            card.BorderBrush(brush(0xFFFFFF, .22)); card.BorderThickness(ThicknessHelper::FromUniformLength(1));
        }
        card.Content(content);
        SlotCard slot{card, body};
        paint_slot(slot, k, false);
        return slot;
    }
    void paint_slot(SlotCard const& slot, size_t k, bool target) {
        const auto held = model.find(model.kept[k]);
        slot.card.Background(brush(0xFFFFFF, target ? .2 : held ? (*held == model.selected ? .2 : .07) : 0));
    }
    // A hovering tile moves things the same way the main-screen row does: whatever it would replace
    // slides out of the target slot, and a slot the EQ would leave empties the same way. Sony presets
    // are already on the earbuds, so a slot never opens for one.
    void mark_drop(int slot, size_t dragged) {
        auto const& profile = model.profiles[dragged];
        const bool allowed = !profile.factory && profile.curve_known;
        if (slot == drop_slot && allowed == drop_allowed) return;
        drop_slot = slot; drop_allowed = allowed;
        const int from = model.kept_slot(dragged);
        for (size_t k = 0; k < slot_cards.size(); ++k) {
            const bool target = allowed && static_cast<int>(k) == slot && from != slot;
            const bool leaving = allowed && slot >= 0 && from == static_cast<int>(k) && from != slot;
            const bool out = (target && model.kept[k]) || leaving;
            paint_slot(slot_cards[k], k, target);
            move_compositor(slot_cards[k].body, 0, out ? static_cast<float>(kTileHeight) : 0.f, 200, live && animations);
        }
    }
    bool animate_library() {
        if (!drawer_open && drawer_t <= 0) return false;
        bool moving = false;
        for (size_t i = 0; i < library.size(); ++i) {
            if (static_cast<int>(i) == drag_index && drag_lifted) continue;
            if(library[i].composition){auto& tile=library[i];if(tile.x!=tile.target_x||tile.y!=tile.target_y){move_compositor(tile.button,static_cast<float>(tile.target_x),static_cast<float>(tile.target_y),200,animations);tile.x=tile.target_x;tile.y=tile.target_y;}continue;}
            auto& tile = library[i];
            const double dx = tile.target_x - tile.x, dy = tile.target_y - tile.y;
            if (std::abs(dx) < .4 && std::abs(dy) < .4) {
                if (dx != 0 || dy != 0) {
                    tile.x = tile.target_x;
                    tile.y = tile.target_y;
                    tile.motion.X(tile.x);
                    tile.motion.Y(tile.y);
                }
                continue;
            }
            tile.x += dx * (1-std::exp(-frame_seconds/.07));
            tile.y += dy * (1-std::exp(-frame_seconds/.07));
            tile.motion.X(tile.x);
            tile.motion.Y(tile.y);
            moving = true;
        }
        return moving;
    }
    void begin_drag(size_t position, Input::PointerRoutedEventArgs const& args) {
        if (searching() || !args.GetCurrentPoint(nullptr).Properties().IsLeftButtonPressed()) return;
        drag_index = static_cast<int>(position);
        drag_lifted = false;
        drag_start = args.GetCurrentPoint(get<Canvas>(L"LibraryCanvas")).Position();
        if(library[position].composition){auto local=args.GetCurrentPoint(library[position].button).Position();library[position].x=drag_start.X-local.X;library[position].y=drag_start.Y-local.Y;move_compositor(library[position].button,static_cast<float>(library[position].x),static_cast<float>(library[position].y),0,false);}
        drag_offset = {drag_start.X - static_cast<float>(library[position].x), drag_start.Y - static_cast<float>(library[position].y)};
    }
    void continue_drag(Input::PointerRoutedEventArgs const& args) {
        if (drag_index < 0) return;
        const auto at = args.GetCurrentPoint(get<Canvas>(L"LibraryCanvas")).Position();
        if (!drag_lifted) {
            if (std::hypot(at.X - drag_start.X, at.Y - drag_start.Y) < 6) return;
            drag_lifted = true;
            Canvas::SetZIndex(library[static_cast<size_t>(drag_index)].button, 10);
            library[static_cast<size_t>(drag_index)].button.Background(brush(0x262626));
        }
        auto& tile = library[static_cast<size_t>(drag_index)];
        tile.x = at.X - drag_offset.X;
        tile.y = at.Y - drag_offset.Y;
        if(tile.composition)move_compositor(tile.button,static_cast<float>(tile.x),static_cast<float>(tile.y),0,false);else{tile.motion.X(tile.x);tile.motion.Y(tile.y);}
        // Over an earbud slot: light it up and keep the library order as it was.
        {
            const double width = tile_width(); int over = -1;
            if (at.Y >= kCrateTop && at.Y <= kCrateTop + kTileHeight)
                for (int k = 0; k < 2; ++k) { const double left = 6 + k * (width + kTileGap); if (at.X >= left && at.X <= left + width) over = k; }
            mark_drop(over, tile.index);
            if (over >= 0) return;
        }
        // Find the slot under the tile's centre and open a gap there.
        size_t best = 0;
        double best_distance = 1e12;
        for (size_t slot = 0; slot < library.size(); ++slot) {
            const auto position = slot_position(slot);
            const double distance = std::hypot(position.X - tile.x, position.Y - tile.y);
            if (distance < best_distance) { best_distance = distance; best = slot; }
        }
        library_order.erase(std::find(library_order.begin(), library_order.end(), static_cast<size_t>(drag_index)));
        library_order.insert(library_order.begin() + static_cast<std::ptrdiff_t>(best), static_cast<size_t>(drag_index));
        for (size_t slot = 0; slot < library_order.size(); ++slot) {
            const auto position = slot_position(slot);
            library[library_order[slot]].target_x = position.X;
            library[library_order[slot]].target_y = position.Y;
        }
        wake();
    }
    void end_drag() {
        if (drag_index < 0) return;
        const int index = drag_index;
        const bool lifted = drag_lifted;
        drag_index = -1;
        drag_lifted = false;
        if (!lifted) return;
        drag_suppress_click = true;
        if (drop_slot >= 0) { const auto k = static_cast<size_t>(drop_slot); drop_slot = -1; if (drop_allowed) model.keep(k, library[static_cast<size_t>(index)].index); library_changed(); return; }
        const auto slot = static_cast<size_t>(std::find(library_order.begin(), library_order.end(), static_cast<size_t>(index)) - library_order.begin());
        model.move(library[static_cast<size_t>(index)].index, slot);
        library_changed();
    }
    void open_drawer() {
        if (!controls_live && !render_review) return; // locked while disconnected (0.8.0)
        if(root.XamlRoot())drawer_focus=Input::FocusManager::GetFocusedElement(root.XamlRoot()).try_as<Control>();
        drawer_open = true;apply_light();
        get<UIElement>(L"MainPage").IsHitTestVisible(false);get<UIElement>(L"EqPage").IsHitTestVisible(false);
        show(L"Drawer", true);
        root.UpdateLayout();
        rebuild_library();
        if(live){
            if(!drawer_composition){get<UIElement>(L"Drawer").RenderTransform(nullptr);move_compositor(get<FrameworkElement>(L"Drawer"),0,static_cast<float>(root.ActualHeight()),240,false);drawer_composition=true;}
            move_compositor(get<FrameworkElement>(L"Drawer"),0,0,240,animations);
        }
        if (animations) { apply_drawer(); wake(); }
        else { drawer_t = 1; apply_drawer(); }
    }
    void close_drawer() {
        drawer_open = false;apply_light();
        if(drawer_focus)drawer_focus.Focus(FocusState::Programmatic);
        get<UIElement>(L"MainPage").IsHitTestVisible(true);get<UIElement>(L"EqPage").IsHitTestVisible(true);
        if(live)move_compositor(get<FrameworkElement>(L"Drawer"),0,static_cast<float>(root.ActualHeight()),240,animations);
        if (animations) wake();
        else { drawer_t = 0; apply_drawer(); }
    }
    void apply_drawer() {
        const double e = 1 - std::pow(1 - drawer_t, 3);
        if(!drawer_composition)drawer_move.Y((1 - e) * std::max(800.0, root.ActualHeight()));
        // The glass is translucent, but only the stage frosts: the controls under it fade away
        // instead, so nothing sharp shows through.
        for (auto const& child : get<Grid>(L"MainPage").Children()) {
            auto element = child.try_as<FrameworkElement>();
            if (element && element.Name() != L"Stage" && element.Name() != L"WaveChain") element.Opacity(1 - e);
        }
        if (!drawer_open && drawer_t <= 0) show(L"Drawer", false);
    }

    // ---------- Wiring ----------
    void click(const wchar_t* name, std::function<void()> action) { get<Button>(name).Click([action](auto&&, auto&&) { action(); }); }
    void note(std::wstring const& text) { get<TextBlock>(L"EqNote").Text(text); }
    // ---------- Wave preview (0.8.0, reworked in 0.8.1) ----------
    // While the Wave choice in Settings is hovered or has keyboard focus, Settings fades away and the
    // main screen shows exactly as you'd see it, moving with your music at the chosen setting; the
    // choice's flyout stays on top. Only while music plays on connected earbuds: without them there
    // is nothing to see. It ends by itself if either stops (tick()).
    // 0.8.2: only the waves show (not the EQ tiles), and the change goes through black in two steps,
    // Settings out then waves in, and back, so the two are never on screen together. The preview
    // starts after resting on the choice for half a second, so passing over it does nothing.
    bool wave_preview = false, preview_shown = false; // shown: the main screen is up (or fading)
    Dispatching::DispatcherQueueTimer preview_timer{nullptr}; // kept: a local timer can be released before it fires (0.8.0 left the preview stuck)
    Dispatching::DispatcherQueueTimer preview_dwell{nullptr};
    std::function<void()> preview_next; // the second half of the change, run by preview_timer
    std::vector<std::pair<FrameworkElement, double>> preview_hidden; // the main screen's controls, and their own opacity
    bool stage_on() const { return page == L"Main" || wave_preview; }
    bool preview_possible() const { return page == L"Settings" && !high_contrast && music_playing && controls_live; }
    static constexpr int kPreviewOut = 150, kPreviewIn = 250;
    void preview_after(int ms, std::function<void()> next) {
        if (!preview_timer) {
            preview_timer = Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer(); preview_timer.IsRepeating(false);
            preview_timer.Tick([this](auto&&, auto&&) { if (auto next = std::move(preview_next)) { preview_next = nullptr; next(); } });
        }
        preview_timer.Stop(); preview_next = std::move(next); preview_timer.Interval(std::chrono::milliseconds(ms)); preview_timer.Start();
    }
    // Hovering asks for the preview after a short rest; keyboard focus asks at once.
    void request_wave_preview(bool on, bool at_once = false) {
        if (!preview_dwell) {
            preview_dwell = Dispatching::DispatcherQueue::GetForCurrentThread().CreateTimer(); preview_dwell.IsRepeating(false); preview_dwell.Interval(std::chrono::milliseconds(500));
            preview_dwell.Tick([this](auto&&, auto&&) { set_wave_preview(true); });
        }
        preview_dwell.Stop();
        if (on && !at_once) preview_dwell.Start(); else set_wave_preview(on);
    }
    void set_wave_preview(bool on) {
        if (on && !preview_possible()) return;
        if (on == wave_preview) return;
        wave_preview = on;
        auto wave = get<UIElement>(L"WaveChain");
        if (on) {
            fade(get<UIElement>(L"SettingsPage"), 0, kPreviewOut);
            preview_after(kPreviewOut, [this, wave] {
                if (!wave_preview) return;
                if (!preview_shown) {
                    preview_shown = true;
                    for (auto const& child : get<Grid>(L"MainPage").Children())
                        if (auto element = child.try_as<FrameworkElement>(); element && element.Name() != L"Stage" && element.Name() != L"WaveChain") { preview_hidden.emplace_back(element, element.Opacity()); element.Opacity(0); }
                    get<UIElement>(L"MainPage").IsHitTestVisible(false); show(L"MainPage", true); root.UpdateLayout(); draw_field(); layout_light(); apply_light(); wake();
                }
                fade(wave, 1, kPreviewIn, 0);
            });
        } else {
            fade(wave, 0, kPreviewOut);
            preview_after(kPreviewOut, [this] { if (!wave_preview) { end_preview_now(false); fade(get<UIElement>(L"SettingsPage"), 1, kPreviewIn); } });
        }
    }
    // Puts the main screen away at once (also when leaving Settings). Only undoes what the preview changed.
    void end_preview_now(bool settings_now = true) {
        if (preview_timer) preview_timer.Stop();
        if (preview_dwell) preview_dwell.Stop();
        preview_next = nullptr; wave_preview = false;
        if (preview_shown) {
            preview_shown = false;
            for (auto& [element, opacity] : preview_hidden) element.Opacity(opacity);
            preview_hidden.clear();
            // The connection may have changed meanwhile: the controls rest exactly as it says.
            for (auto name : {L"NoiseHost", L"Bank", L"AllButton"}) get<FrameworkElement>(name).Opacity(controls_live ? 1 : .4);
            get<UIElement>(L"MainPage").IsHitTestVisible(true); show(L"MainPage", page == L"Main");
        }
        fade(get<UIElement>(L"WaveChain"), 1, 0);
        if (settings_now) fade(get<UIElement>(L"SettingsPage"), 1, 0);
    }
    void navigate(std::wstring const& target_page) {
        if (target_page != L"Settings") end_preview_now();
        page = target_page;
        apply_light();
        show(L"MainPage", page == L"Main");
        show(L"EqPage", page == L"Eq");
        show(L"SettingsPage", page == L"Settings");
        show(L"BackButton", page != L"Main");
        show(L"SettingsButton", page != L"Settings");
        show(L"TitleState", page != L"Settings");
        get<TextBlock>(L"TitleText").Text(page == L"Settings" ? L"Settings" : L"WF-C710N");
        root.UpdateLayout();
        sync_pills(false);
        if (page == L"Settings") layout_settings();
        layout_light();
        apply_curve_glow();
        if (page == L"Main" && timer && animations) wake();
    }
    void open_editor() {
        if (!controls_live && !render_review) return; // locked while disconnected (0.8.0)
        model.editing = true; opened = model; history.clear(); last_kind = -1; comparing = false;
        compare_curve = model.current().saved;
        if (!guide) restore_editor(false); // whatever happened before, the editor opens editable
        sync_eq();
        navigate(L"Eq");
        draw_curve();
    }
    void start_guide_from_home() {
        if (!music_playing) return; // the menu item is disabled then; this is the backstop
        if (drawer_open) close_drawer();
        open_editor(); guide_from_home = true; enter_guide();
    }
    // ---------- Find your EQ ----------
    // The rounds play on the editor page itself. Choosing A or B makes that curve the current one,
    // which on the earbuds is what you would hear, so your ears decide. Nothing is created until Keep.
    // Compare no longer moves the curve: what you made stays, and how it was shows as a dotted afterimage.
    bool guide_before() const { return guide && guide->reviewing() && guide->before; }
    dusk::Curve eq_target() const { return guide_before() ? model.current().saved : guide ? guide->curve() : model.current().draft; }
    void show_curve(bool animate) {
        push_heard();
        if (animate && animations) { eq_from = eq_shown; eq_blend = 0; wake(); return; }
        eq_blend = 1; const auto target = eq_target();
        for (size_t i = 0; i < eq_shown.size(); ++i) eq_shown[i] = target[i];
        draw_curve();
    }
    void fade(UIElement const& element, float to, int milliseconds = 200, float from = -1) {
        if (!live) { element.Opacity(to); return; }
        auto visual = Hosting::ElementCompositionPreview::GetElementVisual(element);
        visual.StopAnimation(L"Opacity");
        if (!animations || milliseconds <= 0) { visual.Opacity(to); return; }
        if (from >= 0) visual.Opacity(from);
        auto animation = visual.Compositor().CreateScalarKeyFrameAnimation(); animation.InsertExpressionKeyFrame(0, L"this.StartingValue");
        animation.InsertKeyFrame(1, to, visual.Compositor().CreateCubicBezierEasingFunction({1.f/3, 1}, {2.f/3, 1}));
        animation.Duration(std::chrono::milliseconds(milliseconds)); visual.StartAnimation(L"Opacity", animation);
    }
    // A new round slides in from the side it comes from, and can be interrupted by the next.
    void slide_in(FrameworkElement const& element, int direction) {
        if (!live || !animations || !direction) return;
        move_compositor(element, 24.f * direction, 0, 240, false); move_compositor(element, 0, 0, 240, true);
        fade(element, 1, 240, 0);
    }
    // The five tiles. Round 1 is Sony's 1 to 5. Round 2 is 6, 7, your pick, 8, 9, so the pick sits in
    // the middle. The result uses the first two tiles as After and Before.
    static int guide_option(int stage, int slot) { static constexpr int second[] = {1, 2, 0, 3, 4}; return stage == 1 ? second[slot] : slot; }
    int guide_slot() const {
        if (!guide) return -1;
        if (guide->reviewing()) return guide->before ? 1 : 0;
        for (int slot = 0; slot < 5; ++slot) if (guide_option(guide->stage, slot) == (guide->stage ? guide->second : guide->first)) return slot;
        return -1;
    }
    static std::wstring guide_name(int slot) { return L"GuideOption" + std::to_wstring(slot); }
    void place_guide_pill(bool animate) {
        const int slot = guide_slot();
        FrameworkElement key = slot >= 0 ? get<FrameworkElement>(guide_name(slot).c_str()) : FrameworkElement{nullptr};
        if (!animate) guide_pill.to_width = 0; // a new round's pill appears in place instead of sliding from the last
        place_pill(guide_pill, key, get<FrameworkElement>(L"GuideChoiceHost"), animate);
    }
    // Clear Bass is part of what the guide changes, so its column stays and follows the curve, but
    // can't be dragged until the guide is over. The bands' sliders and handles are put back after.
    void bass_interactive(bool on) {
        get<UIElement>(L"BassRow").IsHitTestVisible(on); get<Control>(L"BassSlider").IsTabStop(on);
        show(L"BandValues", on);
        if (on) { get<UIElement>(L"BandGrid").IsHitTestVisible(true); fade(get<UIElement>(L"BandGrid"), 1); for (auto const& handle : eq_handles) fade(handle, 1); }
    }
    void sync_guide(int direction) {
        auto const& g = *guide; const bool review = g.reviewing();
        static constexpr const wchar_t* steps[] = {L"Pick a sound", L"A closer match", L"Your EQ"}; // the dots already say which step
        get<TextBlock>(L"GuideStep").Text(guide_paused ? L"Paused · no music playing" : steps[g.stage]); // no instructions: the owner finds them out of keeping
        for (int slot = 0; slot < 5; ++slot) {
            const auto suffix = std::to_wstring(slot);
            std::wstring label = review ? (slot ? L"Before" : L"After")
                               : g.stage == 1 ? (slot == 2 ? std::to_wstring(g.first + 1) : std::to_wstring(guide_option(1, slot) + 5))
                                              : std::to_wstring(slot + 1);
            get<TextBlock>((L"GuideLabel" + suffix).c_str()).Text(label);
            auto tile = get<ToggleButton>(guide_name(slot).c_str());
            show(guide_name(slot).c_str(), !review || slot < 2);
            tile.IsChecked(slot == guide_slot());
            Automation::AutomationProperties::SetName(tile, review ? label : L"Sound " + label);
        }
        show(L"GuideRetryButton", review);
        get<Button>(L"GuideNextButton").Content(box_value(review ? L"Keep" : L"Next"));
        get<Button>(L"GuideNextButton").IsEnabled(!guide_paused);
        for (int slot = 0; slot < 5; ++slot) get<Control>(guide_name(slot).c_str()).IsTabStop(!guide_paused);
        get<Button>(L"GuideBackButton").IsEnabled(g.stage > 0);
        for (size_t i = 0; i < guide_dots.size(); ++i) {
            const bool current = static_cast<int>(i) == g.stage;
            guide_dots[i].Scale({current ? 1.f : 1.f / 3, 1, 1});
            guide_dots[i].Opacity(current ? 1 : static_cast<int>(i) < g.stage ? .7 : .28);
        }
        // The result shows its numbers, like Sony's; the rounds are chosen by ear.
        const auto shown = eq_target();
        auto values = get<UIElement>(L"BandValues");
        const bool appearing = review && values.Visibility() != Visibility::Visible;
        show(L"BandValues", review); if (appearing) fade(values, 1, 240, 0);
        for (size_t i = 0; i < 5; ++i) band_values[i].Text(signed_value(shown[i]));
        get<TextBlock>(L"BassValue").Text(signed_value(shown[5]));
        root.UpdateLayout();
        place_guide_pill(direction == 0);
        for (auto name : {L"GuideStep", L"GuideContent"}) slide_in(get<FrameworkElement>(name), direction);
        show_curve(true);
    }
    // The editor as it is outside Find your EQ: header, readouts, draggable bands and Clear Bass.
    void restore_editor(bool animate) {
        show(L"GuideHeader", false); show(L"GuidePanel", false);
        for (auto name : {L"EqHeader", L"BandValues"}) { show(name, true); if (animate) fade(get<UIElement>(name), 1, 200, 0); else fade(get<UIElement>(name), 1, 0); }
        get<UIElement>(L"BassRow").IsHitTestVisible(true); get<Control>(L"BassSlider").IsTabStop(true);
        get<UIElement>(L"BandGrid").IsHitTestVisible(true); fade(get<UIElement>(L"BandGrid"), 1, animate ? 200 : 0);
        for (auto const& handle : eq_handles) fade(handle, 1, animate ? 200 : 0);
    }
    bool editor_editable() {
        auto visual_opacity = [](UIElement const& e) { return Hosting::ElementCompositionPreview::GetElementVisual(e).Opacity(); };
        return get<UIElement>(L"BandGrid").IsHitTestVisible() && visual_opacity(get<UIElement>(L"BandGrid")) > .99f && get<UIElement>(L"BassRow").IsHitTestVisible()
            && get<UIElement>(L"EqHeader").Visibility() == Visibility::Visible && get<UIElement>(L"GuidePanel").Visibility() == Visibility::Collapsed;
    }
    void enter_guide() {
        guide = dusk::Guide{};
        guide_paused = false; guide_silence_ms = 0;
        get<UIElement>(L"GuideChoiceHost").IsHitTestVisible(true); fade(get<UIElement>(L"GuideChoiceHost"), 1, 0);
        end_compare();
        show(L"EqHeader", false); bass_interactive(false);
        show(L"GuideHeader", true); show(L"GuidePanel", true);
        get<UIElement>(L"BandGrid").IsHitTestVisible(false); fade(get<UIElement>(L"BandGrid"), 0);
        for (auto const& handle : eq_handles) fade(handle, 0);
        for (auto name : {L"GuideHeader", L"GuidePanel"}) fade(get<UIElement>(name), 1, 200, 0);
        sync_guide(0);
        get<Control>(L"GuideOption0").Focus(FocusState::Programmatic);
    }
    // Paused: the choices dim like disconnected controls and stop responding, Next and Keep are off,
    // and the title says why. Cancel, Back and Start again still work; picks are kept.
    void apply_guide_pause() {
        if (!guide) return;
        auto host = get<UIElement>(L"GuideChoiceHost");
        host.IsHitTestVisible(!guide_paused); fade(host, guide_paused ? .4f : 1.f, 200);
        get<TextBlock>(L"GuideStep").Text(guide_paused ? std::wstring(L"Paused · no music playing")
            : std::wstring(guide->stage == 0 ? L"Pick a sound" : guide->stage == 1 ? L"A closer match" : L"Your EQ"));
        get<Button>(L"GuideNextButton").IsEnabled(!guide_paused);
        for (int slot = 0; slot < 5; ++slot) get<Control>(guide_name(slot).c_str()).IsTabStop(!guide_paused);
        if (guide_paused && get<Control>(L"GuideNextButton").FocusState() != FocusState::Unfocused) get<Control>(L"GuideBackButton").Focus(FocusState::Programmatic);
    }
    void choose_guide(int slot) {
        if (!guide || guide_paused) return;
        if (guide->reviewing()) guide->before = slot == 1; else guide->choose(guide_option(guide->stage, slot));
        sync_guide(0);
    }
    void finish_guide(bool keep) {
        if (!guide) return;
        const auto result = guide->curve(); guide.reset();
        const bool from_home = guide_from_home; guide_from_home = false;
        // Always put the editor back first, however the guide ends. Leaving from the library used to
        // return before this, and every EQ opened after that could not be edited (fixed in 0.7.2).
        restore_editor(true);
        if (!keep && from_home) { leave_editor(); return; }
        if (keep) {
            push_history(-5); compare_curve = model.current().saved; // compare the new EQ with what was playing
            model.create(); auto& profile = model.current(); profile.name = L"Personal EQ";
            for (int number = 2; std::count_if(model.profiles.begin(), model.profiles.end(), [&](auto const& item) { return item.name == profile.name; }) > 1; ++number) profile.name = L"Personal EQ " + std::to_wstring(number);
            profile.draft = profile.saved = result; rebuild_banks();
        }
        sync_eq(true); root.UpdateLayout(); sync_pills(false);
        get<Control>(L"CompareButton").Focus(FocusState::Programmatic);
    }
    void bind() {
        for (int slot = 0; slot < 5; ++slot) get<ToggleButton>(guide_name(slot).c_str()).Click([this, slot](auto&&, auto&&) { choose_guide(slot); });
        get<HyperlinkButton>(L"GuideRetryButton").Click([this](auto&&, auto&&) { if (guide) { guide->retry(); sync_guide(-1); get<Control>(L"GuideOption0").Focus(FocusState::Programmatic); } });
        click(L"GuideNextButton", [this] { if (!guide || guide_paused) return; if (guide->reviewing()) finish_guide(true); else { guide->next(); sync_guide(1); } });
        click(L"GuideBackButton", [this] { if (guide && guide->back()) sync_guide(-1); });
        click(L"GuideCancelButton", [this] { finish_guide(false); });
        click(L"EditorNewButton",[this]{model.create();rebuild_banks();open_editor();});
        click(L"SettingsButton", [this] { navigate(L"Settings"); });
        click(L"OfflineReconnect", [this] { reconnect(); });
        click(L"OfflineBluetooth", [] { ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL); });
        click(L"UpdateButton", [this] { install_update(); });
        click(L"BackButton", [this] { if (page == L"Eq") { if (guide) finish_guide(false); else leave_editor(); } else navigate(L"Main"); });
        click(L"AllButton", [this] { open_drawer(); });
        click(L"EqAllButton", [this] { open_drawer(); });
        click(L"CloseDrawerButton", [this] { close_drawer(); });
        get<MenuFlyoutItem>(L"NewGuideAction").Click([this](auto&&, auto&&) { start_guide_from_home(); });
        get<MenuFlyoutItem>(L"NewFlatAction").Click([this](auto&&, auto&&) { model.create(); rebuild_banks(); close_drawer(); open_editor(); retarget(); });
        get<MenuFlyoutItem>(L"NewCopyAction").Click([this](auto&&, auto&&) { if (!model.current().curve_known) return; model.duplicate(); rebuild_banks(); close_drawer(); open_editor(); retarget(); });
        get<Button>(L"NewButton").Flyout().Opening([this](auto&&, auto&&) {
            auto copy = get<MenuFlyoutItem>(L"NewCopyAction"); copy.IsEnabled(model.current().curve_known); copy.Text(L"Copy of " + model.current().name);
            // Find your EQ is chosen by ear, so like Sony's it needs music playing (owner, 5 Nov 2026).
            auto find = get<MenuFlyoutItem>(L"NewGuideAction"); find.IsEnabled(music_playing);
            find.Text(music_playing ? L"Find your EQ (listen and pick)" : L"Find your EQ (play music first)");
        });
        get<TextBox>(L"LibrarySearch").TextChanged([this](auto&&, auto&&) { if (drawer_open) rebuild_library(); });
        root.KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args) {
            if (args.Key() == Windows::System::VirtualKey::Escape && drawer_open) { close_drawer(); args.Handled(true); }
            else if (args.Key() == Windows::System::VirtualKey::Escape && guide) { finish_guide(false); args.Handled(true); }
            else if (args.Key() == Windows::System::VirtualKey::Escape && page == L"Eq") { leave_editor(); args.Handled(true); }
            else if (args.Key() == Windows::System::VirtualKey::Escape && page == L"Settings") { navigate(L"Main"); args.Handled(true); }
        });
        click(L"EditorUndoButton", [this] { if (history.empty()) return; auto state = history.back(); history.pop_back(); restore(state); });
        get<MenuFlyoutItem>(L"ResetAction").Click([this](auto&&, auto&&) { push_history(-2); restore(opened); });
        {
            // Hold to hear the EQ as it was when the editor opened; letting go returns to the edit.
            auto compare = get<Button>(L"CompareButton");
            compare.AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler([this](auto&&, auto&&) { start_compare(); })), true);
            for (auto event : {UIElement::PointerReleasedEvent(), UIElement::PointerCaptureLostEvent(), UIElement::PointerExitedEvent(), UIElement::PointerCanceledEvent()})
                compare.AddHandler(event, box_value(Input::PointerEventHandler([this](auto&&, auto&&) { end_compare(); })), true);
            compare.AddHandler(UIElement::KeyDownEvent(), box_value(Input::KeyEventHandler([this](auto&&, Input::KeyRoutedEventArgs const& args) { if (args.Key() == Windows::System::VirtualKey::Space || args.Key() == Windows::System::VirtualKey::Enter) start_compare(); })), true);
            compare.AddHandler(UIElement::KeyUpEvent(), box_value(Input::KeyEventHandler([this](auto&&, Input::KeyRoutedEventArgs const& args) { if (args.Key() == Windows::System::VirtualKey::Space || args.Key() == Windows::System::VirtualKey::Enter) end_compare(); })), true);
            compare.LostFocus([this](auto&&, auto&&) { end_compare(); });
        }
        get<MenuFlyoutItem>(L"FlatAction").Click([this](auto&&,auto&&) { push_history(-3); commit_curve(dusk::Curve{}); sync_eq(true); });
        get<MenuFlyoutItem>(L"DuplicateAction").Click([this](auto&&,auto&&) { push_history(-3); model.duplicate(); rebuild_banks(); sync_pills(false); sync_eq(); });
        get<MenuFlyoutItem>(L"DeleteAction").Click([this](auto&&,auto&&) {
            push_history(-3);
            model.erase();
            compare_curve = model.current().saved;
            rebuild_banks();
            sync_pills(false);
            sync_eq();
        });
        auto name = get<TextBox>(L"ProfileName");
        name.LostFocus([this](auto&&, auto&&) { rename_current(); });
        name.KeyDown([this](auto&&, Input::KeyRoutedEventArgs const& args) { if (args.Key() == Windows::System::VirtualKey::Enter) { rename_current(); get<Control>(L"CompareButton").Focus(FocusState::Programmatic); args.Handled(true); } });
    }

    // ---------- Offscreen review exports ----------
    // ---------- Owner link ----------
    void load_saved_library() {
        auto saved = earbud::load_library(earbud::data_folder());
        if (saved.profiles.empty()) { // until the earbuds have been read: just Off, so nothing pretends to be real
            saved.profiles.push_back({1, L"Off", earbud::ProfileKind::Sony, 0, true, {}, 0});
            saved.next_id = 2;
        }
        dusk::load_library(model, saved);
        library_imported = saved.imported;
        set_battery(L"BatteryLeft", -1, false); set_battery(L"BatteryCase", -1, false); set_battery(L"BatteryRight", -1, false);
        set_controls_live(false, false);
        get<TextBlock>(L"TitleState").Text(L"Connecting…");
    }
    void start_owner_link() {
        if (render_review || link) return;
        auto queue = Dispatching::DispatcherQueue::GetForCurrentThread();
        const auto make = [&](int ms, auto action) { auto t = queue.CreateTimer(); t.Interval(std::chrono::milliseconds(ms)); t.IsRepeating(false); t.Tick([action](auto&&, auto&&) { action(); }); return t; };
        push_timer = make(120, [this] { send_heard(); });
        library_timer = make(300, [this] { send_library(); });
        keep_timer = make(1500, [this] { send_keeps(); });
        link = std::make_unique<earbud::OwnerLink>(
            [queue](std::function<void()> fn) { queue.TryEnqueue([fn] { fn(); }); },
            [this](std::string text) { owner_message(text); },
            [this](bool connected) { if (connected) send_interest(true); else { owner_ready = false; get<TextBlock>(L"TitleState").Text(L"Starting…"); } });
    }
    void send_json(JsonObject const& message) { if (link) link->send(to_string(message.Stringify())); }
    // What this window is showing, so the owner keeps that device's controls open.
    std::string device_address; // the device on screen
    void send_interest(bool active) {
        JsonObject m; m.SetNamedValue(L"type", JsonValue::CreateStringValue(L"view")); m.SetNamedValue(L"visible", JsonValue::CreateBooleanValue(active));
        m.SetNamedValue(L"screen", JsonValue::CreateStringValue(L"device")); m.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(device_address)));
        window_visible = active; send_json(m);
    }
    bool window_visible = true;    void send_command(const wchar_t* op, int value, std::optional<dusk::Curve> curve = std::nullopt) {
        if (!link || !owner_ready) return;
        JsonObject m;
        m.SetNamedValue(L"type", JsonValue::CreateStringValue(L"command"));
        m.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(device_address)));
        m.SetNamedValue(L"id", JsonValue::CreateNumberValue(++request_id));
        m.SetNamedValue(L"generation", JsonValue::CreateNumberValue(generation));
        m.SetNamedValue(L"op", JsonValue::CreateStringValue(op));
        m.SetNamedValue(L"value", JsonValue::CreateNumberValue(value));
        if (curve) {
            JsonArray bands; for (size_t b = 0; b < 5; ++b) bands.Append(JsonValue::CreateNumberValue((*curve)[b]));
            m.SetNamedValue(L"bands", bands); m.SetNamedValue(L"bass", JsonValue::CreateNumberValue((*curve)[5]));
        }
        send_json(m);
    }
    static std::string curve_key(dusk::Curve const& c) { std::string k = "a"; for (int v : c) k += ":" + std::to_string(v); return k; }
    // The EQ that should be playing right now: a Find your EQ choice, the "how it was" curve while
    // Compare is held, or the chosen EQ. Sony presets are selected on the earbuds; everything else
    // is written to Manual.
    std::pair<std::string, std::function<void()>> heard() {
        if (guide && !guide_before()) { auto c = guide->curve(); return {curve_key(c), [this, c] { send_command(L"apply", 0, c); }}; }
        if (comparing) { auto c = compare_curve; return {curve_key(c), [this, c] { send_command(L"apply", 0, c); }}; }
        if (model.selected >= model.profiles.size()) return {pushed_key, [] {}};
        auto const& p = model.current();
        if (p.factory) { auto preset = static_cast<int>(p.preset); return {"p:" + std::to_string(preset), [this, preset] { send_command(L"preset", preset); }}; }
        auto c = p.saved; return {curve_key(c), [this, c] { send_command(L"apply", 0, c); }};
    }
    void push_heard() { if (link && owner_ready && heard().first != pushed_key && push_timer && !push_timer.IsRunning()) push_timer.Start(); }
    uint64_t last_push = 0;
    void send_heard() { if (!owner_ready) return; auto [key, send] = heard(); if (key == pushed_key) return; pushed_key = key; last_push = GetTickCount64(); send(); }
    void queue_library() { if (link && owner_library && library_timer && !library_timer.IsRunning()) library_timer.Start(); }
    void send_library() {
        auto next = dusk::to_library(model, library_imported);
        if (next == sent_library) return;
        JsonObject m; m.SetNamedValue(L"type", JsonValue::CreateStringValue(L"library")); m.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(device_address))); m.SetNamedValue(L"library", earbud::to_json(next));
        send_json(m);
        // A slot that now holds a different EQ, or whose EQ changed, is rewritten once edits settle.
        for (size_t k = 0; k < 2; ++k) {
            auto const* now = next.find(next.kept[k]); auto const* was = sent_library.find(sent_library.kept[k]);
            if (now && (!was || was->id != now->id || was->bands != now->bands || was->clear_bass != now->clear_bass)) { keep_timer.Stop(); keep_timer.Start(); }
        }
        pending_library = next; sent_library = std::move(next);
    }
    std::array<std::optional<dusk::Curve>, 2> written_slots{};
    void send_keeps() {
        for (size_t k = 0; k < 2; ++k) {
            auto const* p = sent_library.find(sent_library.kept[k]);
            if (!p) continue;
            dusk::Curve c{}; for (size_t b = 0; b < 5; ++b) c[b] = p->bands[b]; c[5] = p->clear_bass;
            if (written_slots[k] == c) continue;
            written_slots[k] = c;
            send_command(L"keep", static_cast<int>(k + 1), c);
        }
    }
    // Sound modes, the six EQs, the library and the editor all rest while there's no connection
    // (the library too since 0.8.0, owner's call: while disconnected, Sony's phone app can change
    // the earbuds, so editing here would drift from what they hold). The waves settle too.
    bool controls_live = true, card_shown = false;
    // explain: show the card. Not while a connection attempt is under way, so a normal connect
    // never flashes it.
    void set_controls_live(bool on, bool explain) {
        if (on != controls_live) {
            controls_live = on;
            // The element's own opacity, set directly. fade() can't be used: this first runs before
            // the window is live, when fade() sets a different property from the one it animates
            // later, which left the controls dimmed. Nor an OpacityTransition: their children move
            // through the compositor, and WinUI crashed with both on the same subtree (0.5.5).
            for (auto name : {L"NoiseHost", L"Bank", L"AllButton"}) {
                auto element = get<FrameworkElement>(name);
                element.IsHitTestVisible(on); element.Opacity(on ? 1 : .4);
            }
            for (auto key : {L"NoiseCancelling", L"NoiseAmbient", L"NoiseOff", L"AllButton"}) get<Control>(key).IsTabStop(on);
            // Anything open when the connection drops closes: the library, Find your EQ and the editor.
            if (!on && live) {
                if (drawer_open) close_drawer();
                if (page == L"Eq") leave_editor();
            }
            if (on) wake();
        }
        // The card over the stage says why and offers a way back, as Tribit Connect does.
        const bool show_card = !on && explain;
        auto card = get<FrameworkElement>(L"OfflineCard");
        if (show_card) {
            auto seen = get<TextBlock>(L"OfflineSeen");
            if (last_seen) {
                std::tm local{}; const std::time_t at = last_seen; localtime_s(&local, &at);
                wchar_t text[96]{}; wcsftime(text, 96, L"Last seen %I:%M %p. The batteries show what was read then.", &local);
                seen.Text(text); seen.Visibility(Visibility::Visible);
            } else seen.Visibility(Visibility::Collapsed);
        }
        if (show_card == card_shown) return;
        card_shown = show_card;
        card.Visibility(show_card ? Visibility::Visible : Visibility::Collapsed);
        card.Opacity(show_card ? 1 : 0);
    }
    int64_t last_seen = 0;
    void reconnect() {
        if (!link) return;
        JsonObject m;
        send_interest(true); // the owner opens the controls by itself once Windows has the device
    }
    // While connected, an earbud with no reading isn't connected; the case has no reading once both
    // buds are out of it.
    void set_battery(const wchar_t* name, int percent, bool connected) {
        const bool known = percent > 0 && percent <= 100;
        const bool bud = name != std::wstring(L"BatteryCase");
        const bool away = connected && bud && !known;
        auto text = get<TextBlock>((std::wstring(name) + L"Text").c_str());
        text.Text(known ? std::to_wstring(percent) : away ? L"Disconnected" : L"—");
        text.FontSize(away ? 12 : 32);
        text.Height(43); text.TextAlignment(TextAlignment::Center); text.VerticalAlignment(VerticalAlignment::Center);
        text.Padding(ThicknessHelper::FromLengths(0, away ? 14 : 0, 0, 0));
        get<FrameworkElement>((std::wstring(name) + L"Bar").c_str()).Width(known ? 72.0 * percent / 100 : 0);
        auto panel = get<FrameworkElement>(name);
        panel.Opacity(known && connected ? 1 : .55); // dimmed readings are the last known ones
        const std::wstring label = std::wstring(name == std::wstring(L"BatteryCase") ? L"Case battery" : name == std::wstring(L"BatteryLeft") ? L"Left earbud battery" : L"Right earbud battery");
        Automation::AutomationProperties::SetName(panel, known ? label + L" " + std::to_wstring(percent) + L" percent" : away ? label + L", earbud disconnected" : label + L" unknown");
    }
    void owner_message(std::string const& text) {
        JsonObject m{nullptr};
        if (!JsonObject::TryParse(to_hstring(text), m)) return;
        const std::wstring type{m.GetNamedString(L"type", L"")};
        if (type == L"show") { from_tray(); return; }
        // The paired Sony devices. Until the Devices screen exists, this window shows the one the owner names.
        if (type == L"devices") {
            const auto shown = to_string(m.GetNamedString(L"shown", L""));
            if (shown != device_address) { device_address = shown; send_interest(window_visible); }
            if (!shown.empty() && m.GetNamedString(L"focused", L"").empty()) { JsonObject f; f.SetNamedValue(L"type", JsonValue::CreateStringValue(L"focus")); f.SetNamedValue(L"address", JsonValue::CreateStringValue(to_hstring(shown))); send_json(f); }
            return;
        }
        if (type != L"snapshot") return;
        if (to_string(m.GetNamedString(L"address", L"")) != device_address) return;        const bool ready = m.GetNamedBoolean(L"ready", false) && m.GetNamedBoolean(L"fresh", false);
        generation = static_cast<uint32_t>(m.GetNamedNumber(L"generation", 0));
        get<TextBlock>(L"TitleState").Text(ready ? L"Connected" : hstring(m.GetNamedString(L"status", L"Not connected")));
        if (ready) last_seen = static_cast<int64_t>(std::time(nullptr));
        else if (!last_seen) last_seen = static_cast<int64_t>(m.GetNamedNumber(L"checked", 0));
        set_controls_live(ready, !ready && !m.GetNamedBoolean(L"pending", false));
        set_battery(L"BatteryLeft", static_cast<int>(m.GetNamedNumber(L"left", -1)), ready);
        set_battery(L"BatteryRight", static_cast<int>(m.GetNamedNumber(L"right", -1)), ready);
        set_battery(L"BatteryCase", static_cast<int>(m.GetNamedNumber(L"case", -1)), ready);
        if (ambient_slider) {
            const bool ambient = ready && m.GetNamedBoolean(L"ambient_supported", false);
            const int level = static_cast<int>(m.GetNamedNumber(L"ambient", -1)), voice = static_cast<int>(m.GetNamedNumber(L"voice", -1));
            // Like the sound modes: the switch and slider move at once, and reports from before the
            // earbuds have applied the change are ignored, so they don't snap back (0.6.1).
            const bool settled = !m.GetNamedBoolean(L"pending", false);
            updating = true;
            if (settled && GetTickCount64() - last_ambient > 1500 && level >= 1 && level <= 20) ambient_slider.Value(level);
            if (settled && GetTickCount64() - last_voice > 1500 && (voice == 0 || voice == 1)) voice_switch.IsOn(voice == 1);
            updating = false;
            ambient_slider.IsEnabled(ambient); voice_switch.IsEnabled(ambient);
        }
        if (about_firmware) { const std::wstring firmware{m.GetNamedString(L"firmware", L"")}; about_firmware.Text(firmware.empty() ? L"—" : firmware); }
        sync_settings(m, ready);
        // The switch shows at once; reports from before the earbuds have applied it are ignored.
        if (ready && m.GetNamedBoolean(L"noise_supported", false) && !m.GetNamedBoolean(L"pending", false) && GetTickCount64() - last_noise > 1500) {
            const int mode = static_cast<int>(m.GetNamedNumber(L"noise", -1));
            if (mode >= 0 && mode <= 2) set_noise(mode == 1 ? dusk::Noise::Cancelling : mode == 2 ? dusk::Noise::Ambient : dusk::Noise::Off, true);
        }
        // The library: the owner's copy wins, except while one of our own changes is on its way.
        if (m.HasKey(L"library") && m.GetNamedValue(L"library").ValueType() == JsonValueType::Object) {
            if (auto incoming = earbud::from_json(std::wstring(m.GetNamedObject(L"library").Stringify()))) {
                library_imported = incoming->imported;
                if (pending_library && *incoming == *pending_library) pending_library.reset();
                if (!pending_library && !guide && page != L"Eq" && *incoming != dusk::to_library(model, library_imported)) {
                    dusk::load_library(model, *incoming);
                    sent_library = *incoming;
                    rebuild_banks(); sync_pills(false); sync_eq();
                    if (drawer_open) rebuild_library();
                }
                if (!owner_library) { sent_library = *incoming; owner_library = true; }
            }
        }
        // What the earbuds are playing becomes the selection, without echoing it back to them.
        const bool was_ready = owner_ready;
        owner_ready = ready;
        // Skipped while our own change is still being written, so the selection never snaps back.
        const bool settling = m.GetNamedBoolean(L"pending", false) || GetTickCount64() - last_push < 1500 || (push_timer && push_timer.IsRunning());
        if (ready && !settling && m.GetNamedBoolean(L"eq_supported", false) && !guide && !comparing && page != L"Eq") {
            const auto preset = static_cast<unsigned>(m.GetNamedNumber(L"preset", 0));
            auto bands = m.GetNamedArray(L"bands");
            dusk::Curve playing{};
            if (bands.Size() == 5) for (uint32_t b = 0; b < 5; ++b) playing[b] = static_cast<int>(bands.GetNumberAt(b));
            playing[5] = static_cast<int>(m.GetNamedNumber(L"bass", 0));
            std::optional<size_t> match;
            for (size_t i = 0; i < model.profiles.size() && !match; ++i) {
                auto const& p = model.profiles[i];
                if (preset == MDR_MANUAL ? (!p.factory && p.saved == playing) : (p.factory && p.preset == preset)) match = i;
            }
            pushed_key = preset == MDR_MANUAL ? curve_key(playing) : "p:" + std::to_string(preset);
            if (match && *match != model.selected) { model.select(*match); refresh_tiles(); sync_pills(was_ready); retarget(); }
        }
    }
    static constexpr unsigned MDR_MANUAL = 24; // the earbuds' Manual slot: the app's live EQ

    // ---------- Updates ----------
    std::optional<earbud::updates::Release> available;
    // force: Check now in Settings, which skips the daily limit and the switch and reports in `status`.
    fire_and_forget check_for_update(bool force = false, Button status = nullptr) {
        auto lifetime = get_strong();
        const auto current = earbud::updates::parse_version(app_version());
        auto settings = earbud::updates::load_settings();
        auto say = [status](std::wstring const& text, bool enabled) { if (status) { status.Content(box_value(text)); status.IsEnabled(enabled); } };
        if (render_review || store_install()) co_return;
        if (!current) { say(L"Development build", true); co_return; } // unpackaged builds have no version to compare
        if (!force && (!settings.check || earbud::updates::now_seconds() - settings.last_check < 24 * 3600)) co_return;
        settings.last_check = earbud::updates::now_seconds();
        earbud::updates::save_settings(settings);
        say(L"Checking…", false);
        auto ui = Dispatching::DispatcherQueue::GetForCurrentThread();
        const auto started = GetTickCount64();
        std::optional<earbud::updates::Release> release; bool failed = false;
        try { release = earbud::updates::newer_release(co_await earbud::updates::latest_json(), *current); } catch (...) { failed = true; } // offline or rate-limited
        // Long enough to read "Checking…", so a second check visibly happens.
        if (status && GetTickCount64() - started < 700) co_await winrt::resume_after(std::chrono::milliseconds(700 - (GetTickCount64() - started)));
        ui.TryEnqueue([this, release, failed, say] {
            if (failed) { say(L"Couldn't check. Try again", true); return; }
            if (!release) { say(L"Up to date", true); return; }
            available = release;
            auto button = get<Button>(L"UpdateButton");
            button.Content(box_value(L"Update to " + release->version));
            button.Visibility(Visibility::Visible);
            say(std::wstring(release->version) + L" is available", true);
        });
        // Then the button goes back to "Check now", ready for another look.
        if (status) { co_await winrt::resume_after(std::chrono::seconds(4)); ui.TryEnqueue([status] { if (status.IsEnabled()) status.Content(box_value(L"Check now")); }); }
    }
    fire_and_forget install_update() {
        auto lifetime = get_strong();
        if (!available) co_return;
        auto button = get<Button>(L"UpdateButton");
        button.IsEnabled(false); button.Content(box_value(L"Downloading…"));
        auto ui = Dispatching::DispatcherQueue::GetForCurrentThread();
        hstring setup;
        try { setup = co_await earbud::updates::download(*available); } catch (...) {}
        ui.TryEnqueue([this, setup, button] {
            if (setup.empty()) { button.IsEnabled(true); button.Content(box_value(L"Update didn't verify. Try again")); return; }
            // Setup closes this app and the widget for the moment it takes, then reopens the app.
            ShellExecuteW(nullptr, L"open", setup.c_str(), L"/update", nullptr, SW_SHOWNORMAL);
        });
    }

    void set_review_state() {
        if (review_view == L"Ambient") set_noise(dusk::Noise::Ambient);
        if (review_view == L"Off") set_noise(dusk::Noise::Off);
        if (review_view == L"Bass") { pulse = {.95, .8, .5, .2, .08, .04, .02}; phase = .12; }
        if (review_view == L"Treble") { pulse = {.08, .1, .2, .35, .7, .9, .85}; phase = .78; }
        if (review_view == L"Full") { pulse = {.75, .55, .7, .6, .8, .55, .75}; phase = .5; }
        if (review_view == L"Eq") { open_editor(); auto curve = model.current().saved; curve[2] = -3; push_history(12); commit_curve(curve); after_change(); }
        if (review_view == L"Factory") { model.select(0); open_editor(); }
        if (review_view == L"Extreme") { model.editing=true; model.current().name=L"A deliberately long custom EQ name";model.current().draft={10,-10,10,-10,10,-10};sync_eq();navigate(L"Eq"); }
        if(review_view==L"Unknown"){model.select(model.profiles.size()-1);model.editing=true;sync_eq();navigate(L"Eq");}
        if (review_view == L"Beyond") { model.select(8);model.editing=true;sync_eq();navigate(L"Eq"); }
        if(review_view==L"New"){model.create();rebuild_banks();open_editor();}
        if(review_view==L"Guide1"||review_view==L"Guide2"||review_view==L"GuideReview"){open_editor();enter_guide();guide->choose(2); // Sony's 3
            if(review_view!=L"Guide1"){guide->next();guide->choose(4);} // then 9
            if(review_view==L"GuideReview")guide->next();
            sync_guide(0);}
        // The 0.7.1 bug: New > Find your EQ, Cancel, then open an EQ. It must open editable.
        // Disconnected: the card centred on the waves; sound modes, the six EQs and All N dimmed.
        if (review_view == L"Offline") { last_seen = static_cast<int64_t>(std::time(nullptr)); set_controls_live(false, true); get<TextBlock>(L"TitleState").Text(L"Not connected"); }
        // Music stops during the guide: paused, choices dimmed and locked (review_silent is set for it).
        if (review_view == L"GuidePaused") { open_editor(); enter_guide(); guide->choose(2); sync_guide(0); guide_silence_ms = 1000; guide_paused = true; apply_guide_pause(); }
        if (review_view == L"GuideCancelled") { start_guide_from_home(); finish_guide(false); model.select(1); open_editor(); }
        if (review_view == L"Settings") navigate(L"Settings");
        blend = 1;
        apply_light();
    }
    struct AudioFrame { double seconds{}, energy{}, phase{}; std::array<double,7> pulse{}; };
    std::vector<AudioFrame> read_timeline() {
        std::vector<AudioFrame> frames;
        if(review_timeline.empty())return frames;
        std::ifstream input(review_timeline);if(!input)throw std::runtime_error("Cannot read audio timeline.");
        std::string line;std::getline(input,line);
        while(std::getline(input,line)){
            std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);AudioFrame frame;unsigned colour{};int playing{};double ignored{};
            row>>frame.seconds>>frame.energy>>frame.phase>>colour>>playing;
            for(int i=0;i<7;++i)row>>ignored;
            for(auto& value:frame.pulse)row>>value;
            if(!row)throw std::runtime_error("Invalid audio timeline row.");frames.push_back(frame);
        }
        if(frames.empty())throw std::runtime_error("Empty audio timeline.");return frames;
    }
    void apply_review_audio(std::vector<AudioFrame> const& frames,double seconds){
        if(frames.empty())return;
        auto it=std::upper_bound(frames.begin(),frames.end(),seconds,[](double t,AudioFrame const& f){return t<f.seconds;});
        if(it!=frames.begin())--it;
        phase=it->phase;pulse=it->pulse;apply_light();
        apply_field(seconds);
    }
    // Development (--readme-shots): the window's client area as Windows composes it, swap chains included.
    void capture_window(fs::path const& file){
        RECT r{};GetClientRect(main_hwnd,&r);const int w=r.right-r.left,h=r.bottom-r.top;if(w<=0||h<=0)return;
        HDC screen=GetDC(nullptr);HDC memory=CreateCompatibleDC(screen);
        BITMAPINFO info{};info.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};void* bits{};
        HBITMAP bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0);auto previous=SelectObject(memory,bitmap);
        PrintWindow(main_hwnd,memory,PW_CLIENTONLY|0x2/*PW_RENDERFULLCONTENT*/);GdiFlush();
        BITMAPFILEHEADER header{};header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+w*h*4;
        std::ofstream out(file,std::ios::binary);out.write(reinterpret_cast<const char*>(&header),sizeof(header));out.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(BITMAPINFOHEADER));out.write(static_cast<const char*>(bits),static_cast<std::streamsize>(w)*h*4);
        SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,screen);
    }
    Windows::Foundation::IAsyncAction capture_review(fs::path const& file,std::ofstream* samples=nullptr,double seconds=0){
        Imaging::RenderTargetBitmap bitmap;
        co_await bitmap.RenderAsync(root,initial_width,initial_height);
        auto pixels=co_await bitmap.GetPixelsAsync();
        auto data=Windows::Storage::Streams::DataReader::FromBuffer(pixels);
        std::vector<uint8_t> bytes(pixels.Length());data.ReadBytes(bytes);
        if(samples){
            // Fixed empty scene region above the earbuds: captures composited
            // pixels, not just phase or brush properties.
            double r=0,g=0,b=0;int count=0;
            const double scale=static_cast<double>(bitmap.PixelWidth())/initial_width;
            int middle=100;if(page==L"Main"){auto stage=get<Grid>(L"Stage");auto at=stage.TransformToVisual(root).TransformPoint({static_cast<float>(field_x),static_cast<float>(field_y)});middle=static_cast<int>(at.Y);}
            for(int y=middle-20;y<middle+20;++y)for(int x=initial_width/2-30;x<initial_width/2+30;++x){auto at=(static_cast<int>(y*scale)*bitmap.PixelWidth()+static_cast<int>(x*scale))*4;b+=bytes[at];g+=bytes[at+1];r+=bytes[at+2];++count;}
            *samples<<seconds<<','<<phase<<','<<dusk::spectrum_color(phase)<<','<<r/count<<','<<g/count<<','<<b/count<<','<<line_sequence<<','<<stage.sequence<<','<<stage.presents<<'\n';
        }
        Windows::Storage::Streams::InMemoryRandomAccessStream stream;
        auto encoder=co_await Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(),stream);
        encoder.SetPixelData(Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,Windows::Graphics::Imaging::BitmapAlphaMode::Premultiplied,bitmap.PixelWidth(),bitmap.PixelHeight(),96,96,bytes);
        encoder.BitmapTransform().InterpolationMode(Windows::Graphics::Imaging::BitmapInterpolationMode::Fant);
        encoder.BitmapTransform().ScaledWidth(initial_width);encoder.BitmapTransform().ScaledHeight(initial_height);
        co_await encoder.FlushAsync();
        auto reader=Windows::Storage::Streams::DataReader(stream.GetInputStreamAt(0));co_await reader.LoadAsync(static_cast<uint32_t>(stream.Size()));
        std::vector<uint8_t> png(static_cast<size_t>(stream.Size()));reader.ReadBytes(png);
        std::ofstream output(file,std::ios::binary);output.write(reinterpret_cast<const char*>(png.data()),static_cast<std::streamsize>(png.size()));
        if(!output)throw std::runtime_error("Cannot write review image.");
    }
    void export_layout(fs::path const& file){
        std::ofstream out(file);out<<"name,x,y,width,height,handle_error\n";
        for(auto name: page==L"Eq"?std::vector<const wchar_t*>{L"ProfileName",L"CompareButton",L"EditorUndoButton",L"CurveArea",L"BandValues",L"BassSlider",L"EqMoreButton"}:std::vector<const wchar_t*>{L"HomeBankScroll",L"AllButton"}){
            auto element=get<FrameworkElement>(name);auto at=element.TransformToVisual(root).TransformPoint({0,0});
            out<<to_string(name)<<','<<at.X<<','<<at.Y<<','<<element.ActualWidth()<<','<<element.ActualHeight()<<",0\n";
            if(at.X<-.75||at.Y<-.75||at.X+element.ActualWidth()>initial_width+.75||at.Y+element.ActualHeight()>initial_height+.75)throw std::runtime_error("A review control leaves the viewport.");
        }
        if(page!=L"Eq")return;
        std::function<FrameworkElement(DependencyObject const&)> find_thumb=[&](DependencyObject const& parent)->FrameworkElement{
            if(auto fe=parent.try_as<FrameworkElement>();fe&&fe.Name()==L"VerticalThumb")return fe;
            for(int i=0;i<VisualTreeHelper::GetChildrenCount(parent);++i){auto found=find_thumb(VisualTreeHelper::GetChild(parent,i));if(found)return found;}return nullptr;
        };
        auto canvas=get<Canvas>(L"CurveCanvas");dusk::Plot plot{canvas.ActualWidth()-40,canvas.ActualHeight()};
        for(size_t i=0;i<5;++i){
            auto thumb=find_thumb(band_sliders[i]);if(!thumb)throw std::runtime_error("Slider thumb not found.");
            auto at=thumb.TransformToVisual(canvas).TransformPoint({static_cast<float>(thumb.ActualWidth()/2),static_cast<float>(thumb.ActualHeight()/2)});
            double error=std::hypot(at.X-(32+plot.x(i)),at.Y-plot.y(band_sliders[i].Value()));
            out<<"band"<<i<<','<<at.X<<','<<at.Y<<','<<thumb.ActualWidth()<<','<<thumb.ActualHeight()<<','<<error<<'\n';
            if(error>1)throw std::runtime_error("A curve handle and slider thumb are misaligned.");
        }
    }
    fire_and_forget export_review() {
        auto lifetime = get_strong();
        apartment_context ui;
        try {
            root.Measure({static_cast<float>(initial_width), static_cast<float>(initial_height)});
            root.Arrange({0, 0, static_cast<float>(initial_width), static_cast<float>(initial_height)});
            root.UpdateLayout();
            draw_field();apply_light();
            draw_curve();
            sync_pills(false);
            if (review_view == L"Library" || review_view == L"LibraryDrop") {
                open_drawer();
                if(review_motion){
                    animations=true;timer.Stop();
                    auto opening=directory.parent_path().parent_path()/L"review"/review_pass;fs::create_directories(opening);
                    std::ofstream geometry(opening/L"library-opening.csv");geometry<<"ms,width,count\n";
                    for(int ms:{0,16,50,100,200,300}){drawer_t=std::min(1.0,ms/240.0);apply_drawer();root.UpdateLayout();geometry<<ms<<','<<tile_width()<<','<<library.size()<<'\n';co_await resume_after(std::chrono::milliseconds(30));co_await ui;co_await capture_review(opening/(L"library-opening-"+std::to_wstring(ms)+L".png"));}
                    animations=false;
                }
            }
            if (review_view == L"Settings") layout_settings();
            co_await resume_after(std::chrono::milliseconds(900)); // decode the earbud art
            co_await ui;
            root.UpdateLayout();
            draw_curve();
            sync_pills(false);
            if (review_view == L"Library" || review_view == L"LibraryDrop") layout_library(false);
            if (review_view == L"LibraryDrop") { root.UpdateLayout(); mark_drop(0, 6); } // Quiet desk hovering over Custom 1
            if (review_view == L"Settings") layout_settings();
            co_await resume_after(std::chrono::milliseconds(150));
            co_await ui;
            auto folder = directory.parent_path().parent_path() / L"review" / review_pass;
            fs::create_directories(folder);
            std::wstring view = review_view;
            std::transform(view.begin(), view.end(), view.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            const auto filename = view + L"-" + std::to_wstring(initial_width) + L"x" + std::to_wstring(initial_height);
            if(readme_shots){
                // The README's screenshots: the window as Windows shows it (PrintWindow with the full
                // composition, so the swap chains are in it), with demo music and sample batteries.
                set_battery(L"BatteryLeft",82,true);set_battery(L"BatteryRight",79,true);set_battery(L"BatteryCase",61,true);set_controls_live(true,false);
                animations=true;demo_music=true;glow_level=1;last_tick=precise_ms();
                navigate(L"Main");set_noise(dusk::Noise::Ambient);
                for(int i=0;i<150;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                capture_window(folder/L"readme-main.bmp");
                open_drawer();for(int i=0;i<70;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                capture_window(folder/L"readme-library.bmp");
                close_drawer();for(int i=0;i<40;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                open_editor();{auto curve=model.current().saved;curve[0]=3;curve[2]=-2;curve[3]=4;curve[5]=4;commit_curve(curve);after_change();}
                for(int i=0;i<120;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                capture_window(folder/L"readme-editor.bmp");
                start_compare();for(int i=0;i<30;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                capture_window(folder/L"readme-compare.bmp");end_compare();
                enter_guide();choose_guide(2);for(int i=0;i<90;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                capture_window(folder/L"readme-guide.bmp");finish_guide(false);
                navigate(L"Settings");root.UpdateLayout();layout_settings();
                for(int i=0;i<30;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                capture_window(folder/L"readme-settings.bmp");
                // 0.8.0 review: the App and widget rows (Check now, Wave), then the Wave preview.
                {auto app=get<Button>(L"AppButton");app.Flyout().ShowAt(app);
                 for(int i=0;i<30;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                 capture_window(folder/L"review-settings-app.bmp");
                 set_wave_preview(true);
                 for(int i=0;i<60;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                 capture_window(folder/L"review-wave-preview.bmp");
                 app.Flyout().Hide();set_wave_preview(false);
                 for(int i=0;i<30;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                 capture_window(folder/L"review-after-preview.bmp");} // 0.8.0's preview stayed stuck here
                animations=false;demo_music=false;navigate(L"Main");
            }
            if(stage_bench){
                animations=true;demo_music=true;glow_level=1;navigate(L"Main");set_noise(dusk::Noise::Ambient);last_tick=precise_ms();
                const auto until=GetTickCount64()+25000;
                while(GetTickCount64()<until){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                stage.capture=folder/L"stage.bmp";tick();timer.Stop();unpace(); // the last frame, to look at
                open_drawer();for(int i=0;i<40;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                stage.capture=folder/L"stage-frosted.bmp";tick();timer.Stop();unpace();close_drawer();drawer_t=0;apply_drawer();
                open_editor();model.edit(0,4);model.edit(2,-3);sync_eq(true);
                for(int i=0;i<60;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                editor_chain.capture=folder/L"editor.bmp";draw_editor();
                start_compare();for(int i=0;i<20;++i){tick();timer.Stop();unpace();co_await resume_after(std::chrono::milliseconds(16));co_await ui;}
                editor_chain.capture=folder/L"editor-compare.bmp";draw_editor();end_compare();
                animations=false;demo_music=false;
            }
            if(profile_motion){
                costs.clear();animations=true;demo_music=!profile_silent;last_tick=precise_ms();
                std::ofstream intervals(folder/L"intervals.csv");intervals<<"frame,interval_ms\n";auto previous=GetTickCount64();
                for(int frame=0;frame<360;++frame){
                    if(frame%60==0){model.editing=false;navigate(L"Main");set_noise(frame%120?dusk::Noise::Ambient:dusk::Noise::Cancelling);select_profile((frame/60)%6);}
                    if(frame%60==15){open_editor();model.edit(2,(frame/60)%9-4);sync_eq(true);}
                    if(frame%60==30){open_drawer();model.move(7,8);library_changed();}
                    if(frame%60==45)close_drawer();
                    tick();timer.Stop();auto now=GetTickCount64();intervals<<frame<<','<<now-previous<<'\n';previous=now;
                    co_await resume_after(std::chrono::milliseconds(16));co_await ui;
                }
                std::ofstream summary(folder/L"costs.csv");summary<<"operation,milliseconds\n";for(auto const& sample:costs)summary<<sample.name<<','<<sample.ms<<'\n';
                animations=false;demo_music=false;timer.Stop();model.editing=false;drawer_open=false;drawer_t=0;show(L"Drawer",false);navigate(L"Main");
            }
            {std::ofstream state(folder/(filename+L"-state.csv"));state<<"page,glow_visible,all_label,eq_all_label,editable,guide_paused\n"<<to_string(page)<<','<<(glow_level>0)<<','<<to_string(get<TextBlock>(L"AllLabel").Text())<<','<<to_string(get<TextBlock>(L"EqAllLabel").Text())<<','<<(page==L"Eq"?(editor_editable()?"yes":"no"):"-")<<','<<(guide?(guide_paused&&!get<Button>(L"GuideNextButton").IsEnabled()&&!get<UIElement>(L"GuideChoiceHost").IsHitTestVisible()?"yes":"no"):"-")<<'\n';}
            auto audio=read_timeline();
            export_layout(folder/(filename+L"-layout.csv"));
            std::ofstream samples(folder/(filename+L"-pixels.csv"));samples<<"seconds,phase,rgb,pixel_r,pixel_g,pixel_b,line_sequence,glow_sequence,surface_allocations\n";
            if(review_wave_motion){
                auto original=model;double base=30;std::ofstream frames(folder/L"wave-frames.csv");frames<<"case,ms,line_sequence,glow_sequence,surface_allocations,width,height,blend,left_x,right_x\n";
                const std::array<std::pair<dusk::Noise,dusk::Noise>,6> transitions{{{dusk::Noise::Cancelling,dusk::Noise::Ambient},{dusk::Noise::Ambient,dusk::Noise::Off},{dusk::Noise::Off,dusk::Noise::Ambient},{dusk::Noise::Ambient,dusk::Noise::Cancelling},{dusk::Noise::Cancelling,dusk::Noise::Off},{dusk::Noise::Off,dusk::Noise::Cancelling}}};
                for(int scenario=0;scenario<10;++scenario){
                    model=original;model.editing=false;animations=false;set_noise(scenario<6?transitions[scenario].first:dusk::Noise::Cancelling);apply_field(base);animations=true;timer.Stop();
                    if(scenario<6)set_noise(transitions[scenario].second);
                    if(scenario==6)set_noise(dusk::Noise::Ambient);
                    if(scenario==9){model.current().saved={-5,4,-3,5,-2,0};retarget();}
                    timer.Stop();double origin=0;
                    for(int ms:{0,100,200,300,450,650,900,1200}){
                        if(scenario==6&&(ms==200||ms==300)){set_noise(ms==200?dusk::Noise::Off:dusk::Noise::Cancelling);timer.Stop();origin=ms;}
                        blend=scenario==7||scenario==8?1:std::clamp((ms-origin)/900.0,0.0,1.0);
                        if(scenario==7)pulse.fill(0);
                        else if(!audio.empty())apply_review_audio(audio,base+ms/1000.0);
                        apply_field(base+ms/1000.0);apply_light();
                        frames<<scenario<<','<<ms<<','<<line_sequence<<','<<stage.sequence<<','<<stage.presents<<','<<wave_frame.width<<','<<wave_frame.height<<','<<blend<<','<<wave_frame.lines[0].front().x<<','<<wave_frame.lines[0].back().x<<'\n';
                        co_await resume_after(std::chrono::milliseconds(30));co_await ui;co_await capture_review(folder/(L"wave-"+std::to_wstring(scenario)+L"-"+std::to_wstring(ms)+L".png"));
                    }
                }
                const int original_width=initial_width,original_height=initial_height;
                std::ofstream resizing(folder/L"wave-resizes.csv");resizing<<"width,height,line_sequence,glow_sequence,allocations\n";
                for(auto size:{std::pair{960,620},std::pair{1280,800},std::pair{1462,866}}){
                    initial_width=size.first;initial_height=size.second;root.Width(initial_width);root.Height(initial_height);RectangleGeometry bounds;bounds.Rect({0,0,float(initial_width),float(initial_height)});root.Clip(bounds);root.Measure({float(initial_width),float(initial_height)});root.Arrange({0,0,float(initial_width),float(initial_height)});root.UpdateLayout();draw_field();apply_field(base);apply_light();
                    resizing<<initial_width<<','<<initial_height<<','<<line_sequence<<','<<stage.sequence<<','<<stage.presents<<'\n';co_await resume_after(std::chrono::milliseconds(30));co_await ui;co_await capture_review(folder/(L"resize-"+std::to_wstring(initial_width)+L".png"));
                }
                initial_width=original_width;initial_height=original_height;root.Width(initial_width);root.Height(initial_height);RectangleGeometry restored;restored.Rect({0,0,float(initial_width),float(initial_height)});root.Clip(restored);root.Measure({float(initial_width),float(initial_height)});root.Arrange({0,0,float(initial_width),float(initial_height)});root.UpdateLayout();draw_field();
                model=original;animations=false;set_noise(dusk::Noise::Off);apply_field(0);apply_light();co_await capture_review(folder/L"reduced-motion.png");
                navigate(L"Eq");root.UpdateLayout();apply_light();co_await capture_review(folder/L"page-eq.png");navigate(L"Main");root.UpdateLayout();draw_field();apply_field(base);apply_light();co_await capture_review(folder/L"return-home.png");
                model=original;sync_eq();animations=false;set_noise(original.noise);apply_field(base);apply_light();
            }
            const int count=review_sequence?181:1;
            for(int frame=0;frame<count;++frame){
                const double seconds=review_time+frame*.5;apply_review_audio(audio,seconds);root.UpdateLayout();apply_light();
                co_await resume_after(std::chrono::milliseconds(30));co_await ui;
                const auto suffix=review_sequence?L"-frame-"+std::to_wstring(frame):L"";
                co_await capture_review(folder/(filename+suffix+L".png"),&samples,seconds);
            }
            if(review_motion){
                animations=true;select_profile(2);timer.Stop();
                std::ofstream motion(folder/(filename+L"-motion.csv"));motion<<"milliseconds,x,target,width\n";
                for(int ms:{0,40,80,120,200,320}){
                    if(ms==120){select_profile(4);timer.Stop();}
                    for(auto pill:{&bank_pill,&eq_pill}){pill->t=ms<120?ms/200.0:(ms-120)/200.0;pill->apply();}
                    auto const& active=page==L"Eq"?eq_pill:bank_pill;
                    motion<<ms<<','<<active.x<<','<<active.to_x<<','<<active.width<<'\n';
                    root.UpdateLayout();co_await resume_after(std::chrono::milliseconds(30));co_await ui;
                    co_await capture_review(folder/(filename+L"-motion-"+std::to_wstring(ms)+L".png"));
                }
                for(auto [state,name]:{std::pair{L"PointerOver",L"hover"},std::pair{L"Pressed",L"pressed"},std::pair{L"Normal",L"normal"}}){
                    auto key=page==L"Eq"?bank_chips[model.selected]:bank_tiles[model.selected];
                    VisualStateManager::GoToState(key,state,false);
                    root.UpdateLayout();co_await resume_after(std::chrono::milliseconds(30));co_await ui;
                    co_await capture_review(folder/(filename+L"-"+name+L".png"));
                }
            }
            std::ofstream(folder / (filename + L".txt")) << "PASS: native XAML offscreen export; " << initial_width << "x" << initial_height << "; "<<count<<" frames. Native mask brush captured; this is not an on-screen frame-pacing measurement. Not a live design approval.\n";
        } catch (hresult_error const& e) {
            std::ofstream(directory / L"dusk-errors.log") << "Export: " << to_string(e.message()) << '\n';
        } catch (std::exception const& e) {
            std::ofstream(directory / L"dusk-errors.log") << "Export: " << e.what() << '\n';
        }
        if (window) window.Close();
        Exit();
    }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command, int) {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    directory = fs::path(executable).parent_path();
    const std::wstring args(command);
    profile_silent=args.find(L"--profile-silent")!=std::wstring::npos;
    profile_motion=args.find(L"--profile-motion")!=std::wstring::npos;
    render_review = args.find(L"--render-review") != std::wstring::npos;
    demo_music = args.find(L"--demo-music") != std::wstring::npos;
    start_in_tray = args.find(L"--tray") != std::wstring::npos;
    log_audio = args.find(L"--log-audio") != std::wstring::npos;
    const auto option = [&](std::wstring const& key, std::wstring const& fallback) {
        auto pos = args.find(key);
        if (pos == std::wstring::npos) return fallback;
        pos += key.size();
        if(pos<args.size()&&args[pos]==L'\"'){++pos;return args.substr(pos,args.find(L'\"',pos)-pos);}
        return args.substr(pos, args.find(L' ', pos) - pos);
    };
    review_view = option(L"--view=", L"Main");
    review_silent = review_view == L"GuidePaused";
    review_pass = option(L"--pass=", L"pass1");    review_timeline=option(L"--audio-timeline=",L"");
    review_time=std::stod(option(L"--audio-time=",L"30"));
    review_wave_motion=args.find(L"--wave-motion-review")!=std::wstring::npos;
    stage_bench=args.find(L"--stage-bench")!=std::wstring::npos;
    readme_shots=args.find(L"--readme-shots")!=std::wstring::npos;
    if(readme_shots){initial_width=1200;initial_height=760;}
    review_sequence=args.find(L"--sequence")!=std::wstring::npos;
    review_motion=args.find(L"--motion-review")!=std::wstring::npos;
    if((!review_timeline.empty()||review_sequence||review_motion)&&!render_review)return 2;
    if (review_pass.empty() || review_pass.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-") != std::wstring::npos) return 2;
    if(render_review&&args.find(L"--max-review")!=std::wstring::npos){RECT area{};SystemParametersInfoW(SPI_GETWORKAREA,0,&area,0);double dpi=GetDpiForSystem()/96.0;initial_width=static_cast<int>((area.right-area.left)/dpi);initial_height=static_cast<int>((area.bottom-area.top)/dpi);}
    if (args.find(L"--compact") != std::wstring::npos) { initial_width = 960; initial_height = 620; }
    // Review only: --width=N renders at that many DIPs wide (Settings layout checks, 0.8.3).
    if (render_review) if (const auto width = option(L"--width=", L""); !width.empty()) initial_width = std::clamp(_wtoi(width.c_str()), 640, 3000);
    bool bootstrapped = false;
    int result = 0;
    try {
        init_apartment(apartment_type::single_threaded);
        // Installed from the MSIX, the package's framework dependency supplies the runtime. Only a
        // local, unpackaged build run needs the (delay-loaded) bootstrapper to find it.
        UINT32 length = 0;
        if (GetCurrentPackageFullName(&length, nullptr) == APPMODEL_ERROR_NO_PACKAGE) {
            check_hresult(MddBootstrapInitialize2(0x00020005, nullptr, PACKAGE_VERSION{}, MddBootstrapInitializeOptions_None)); // Windows App SDK 2.5, stable
            bootstrapped = true;
        }
        Application::Start([](auto&&) { make<DuskApp>(); });
    } catch (hresult_error const& e) {
        std::ofstream(directory / L"dusk-errors.log") << "HRESULT 0x" << std::hex << static_cast<uint32_t>(e.code().value) << ": " << to_string(e.message()) << '\n';
        result = 1;
    } catch (std::exception const& e) {
        std::ofstream(directory / L"dusk-errors.log") << e.what() << '\n';
        result = 1;
    }
    if (bootstrapped) MddBootstrapShutdown();
    return result;
}
