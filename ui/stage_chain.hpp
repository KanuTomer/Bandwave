#pragma once
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d3dcompiler.h>
#include <vector>
#include <d2d1effects.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <chrono>
#include "wave_frame.hpp"
#include "line_pipeline.hpp"

// GPU timers (diagnostics, off unless "gpu-timing.on" exists in the data folder). Timestamp queries
// around each part of a stage frame, read back a few frames later so nothing waits. Every 5 seconds
// one CSV row: frames per second and the average GPU milliseconds per part. These are times on the
// GPU's clock between the marks, so they include any time the GPU waits for the CPU; for the GPU's
// busy share, compare runs with Windows' "GPU Engine" utilisation counter, as Task Manager does.
// Development: run the app with --render-review --stage-bench to log 25 s of the stage with demo music.
struct StageTimers {
    static constexpr int kMarks=4; // start, glow strokes, Direct2D (blurs, clear, glow upscale), crisp lines
    struct Slot{winrt::com_ptr<ID3D11Query> disjoint;std::array<winrt::com_ptr<ID3D11Query>,kMarks> stamp;bool pending=false,glowing=false;};
    std::filesystem::path log;
    winrt::com_ptr<ID3D11DeviceContext> immediate;
    std::array<Slot,6> ring;int next=-1;
    std::array<double,kMarks-1> sum{};double cpu_sum=0;int frames=0,glow_frames=0,done=0;
    std::chrono::steady_clock::time_point window=std::chrono::steady_clock::now();
    bool on() const {return !log.empty();}
    void create(ID3D11Device* d3d){
        if(!on())return;
        immediate=nullptr;d3d->GetImmediateContext(immediate.put());
        for(auto& slot:ring){D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};slot.disjoint=nullptr;winrt::check_hresult(d3d->CreateQuery(&desc,slot.disjoint.put()));
            desc.Query=D3D11_QUERY_TIMESTAMP;for(auto& stamp:slot.stamp){stamp=nullptr;winrt::check_hresult(d3d->CreateQuery(&desc,stamp.put()));}slot.pending=false;}
        if(!std::filesystem::exists(log)){std::ofstream(log)<<"time,frames_per_second,glow_share,glow_strokes_ms,blurs_and_upscale_ms,crisp_lines_ms,gpu_total_ms,cpu_draw_ms,gpu_busy_percent_estimate\n";}
    }
    void collect(){
        for(auto& slot:ring){
            if(!slot.pending)continue;D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            if(immediate->GetData(slot.disjoint.get(),&disjoint,sizeof(disjoint),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
            std::array<UINT64,kMarks> t{};bool ready=true;
            for(int i=0;i<kMarks;++i)if(immediate->GetData(slot.stamp[i].get(),&t[i],sizeof(UINT64),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)ready=false;
            if(!ready)continue;slot.pending=false;
            if(disjoint.Disjoint||!disjoint.Frequency)continue;
            for(int i=0;i+1<kMarks;++i)sum[i]+=static_cast<double>(t[i+1]-t[i])*1000.0/static_cast<double>(disjoint.Frequency);
            ++done;
        }
    }
    void begin(){if(!on())return;collect();next=(next+1)%static_cast<int>(ring.size());auto& slot=ring[next];if(slot.pending){next=-1;return;}immediate->Begin(slot.disjoint.get());immediate->End(slot.stamp[0].get());}
    void stamp(int index){if(!on()||next<0)return;immediate->End(ring[next].stamp[index].get());}
    void end(bool glowing,double cpu_ms){
        if(!on())return;
        if(next>=0){immediate->End(ring[next].disjoint.get());ring[next].pending=true;}
        ++frames;glow_frames+=glowing;cpu_sum+=cpu_ms;
        const auto now=std::chrono::steady_clock::now();const double seconds=std::chrono::duration<double>(now-window).count();
        if(seconds<5||done==0)return;
        std::ofstream out(log,std::ios::app);const double fps=frames/seconds;double total=0;for(double v:sum)total+=v/done;
        out<<std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()<<','<<fps<<','<<static_cast<double>(glow_frames)/frames;
        for(double v:sum)out<<','<<v/done;out<<','<<total<<','<<cpu_sum/frames<<','<<total*fps/10<<'\n';
        sum={};cpu_sum=0;frames=glow_frames=done=0;window=now;
    }
};

// The main stage, lines and glow together, drawn into one DirectX swap chain and presented once per
// frame (0.5.14). Until 0.5.13 the glow was a CompositionDrawingSurface redrawn every frame and the
// lines were 15 XAML Polylines. Redrawing drawing surfaces at frame rate keeps every pending update
// (microsoft-ui-xaml#12540, Windows App SDK 2.6.5): the app held 2.2 GB of shared GPU memory and
// grew about 5 MB a second (4 Nov 2026). A swap chain is the path meant for content redrawn every
// frame. The stage behind it is solid black, so the chain is opaque.
//
// Same look as before: the glow is the lines' union blurred twice (a tight halo and a wide bloom),
// drawn at half resolution and tinted with the music's colour; the crisp lines go on top at full
// resolution, each with its own opacity and width.
struct StageChain {
    winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel panel{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<IDXGISwapChain1> chain;
    winrt::com_ptr<ID2D1Factory1> factory;
    winrt::com_ptr<ID2D1Device> device;
    winrt::com_ptr<ID2D1DeviceContext> context;
    winrt::com_ptr<ID2D1Bitmap1> target, glow_in, glow_out;
    winrt::com_ptr<ID2D1Effect> blur,normalise,halo,halo_gain,glow,tint,frost_blur;
    // The glow and the frost fade to black at the stage's top and bottom: frosted, the waves reached the
    // bottom edge and stopped there in a hard line under the library (0.5.18).
    winrt::com_ptr<ID2D1LinearGradientBrush> fade_down,fade_up;static constexpr float kBottomFade=96;
    // Frost (0.5.18): behind the library's glass the stage keeps moving, but out of focus. The lines
    // are drawn at a quarter of the resolution, blurred, and scaled up; Windows' own Acrylic can't
    // sample a swap chain, so the stage frosts itself. 0 is sharp, 1 fully frosted.
    float frost=0;static constexpr float kFrostResolution=.25f,kFrostDeviation=20;
    winrt::com_ptr<ID3D11Texture2D> frost_texture;winrt::com_ptr<ID3D11RenderTargetView> frost_view;winrt::com_ptr<ID2D1Bitmap1> frost_in,frost_out;
    static constexpr float kStroke=3,kHaloDeviation=5,kHaloGain=3.4f,kBloomDeviation=24,kBloomGain=1.6f;
    static constexpr float kGlowResolution=.5f; // the glow is only blur, so half resolution looks the same
    float bloom_scale=1;
    unsigned pixel_width=0,pixel_height=0;
    float scale_x=1,scale_y=1;
    D2D1_COLOR_F glow_colour{0,0,0,0};
    uint64_t sequence=0,presents=0;
    StageTimers timers;
    LinePipeline lines; // the line shader (line_pipeline.hpp)
    winrt::com_ptr<ID3D11RenderTargetView> back_view,glow_view;winrt::com_ptr<ID3D11Texture2D> glow_texture;
    winrt::com_ptr<ID3D11DeviceContext> d3dc;
    std::filesystem::path capture; // development (--stage-bench): the next frame is saved here as a BMP
    void save_capture(){save_chain_capture(chain.get(),d3d.get(),d3dc.get(),capture);capture.clear();}
    void attach(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& value){panel=value;}
    void reset(){
        if(context)context->SetTarget(nullptr);
        target=nullptr;glow_in=nullptr;glow_out=nullptr;blur=normalise=halo=halo_gain=glow=tint=frost_blur=nullptr;fade_down=nullptr;fade_up=nullptr;frost_texture=nullptr;frost_view=nullptr;frost_in=nullptr;frost_out=nullptr;
        back_view=nullptr;glow_view=nullptr;glow_texture=nullptr;lines=LinePipeline{};d3dc=nullptr;
        context=nullptr;device=nullptr;factory=nullptr;chain=nullptr;d3d=nullptr;pixel_width=pixel_height=0;
    }
    void create(unsigned w,unsigned h){
        if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d3d.put(),nullptr,nullptr)))
            winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d3d.put(),nullptr,nullptr));
        D2D1_FACTORY_OPTIONS options{};
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,factory.put_void()));
        auto dxgi=d3d.as<IDXGIDevice>();winrt::check_hresult(factory->CreateDevice(dxgi.get(),device.put()));
        winrt::check_hresult(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.put()));
        winrt::com_ptr<IDXGIAdapter> adapter;winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> dxgi_factory;winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),dxgi_factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width=w;desc.Height=h;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount=2;desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        winrt::check_hresult(dxgi_factory->CreateSwapChainForComposition(d3d.get(),&desc,nullptr,chain.put()));
        // Never queue more than one frame: an update always shows the newest lines.
        if(auto device1=d3d.try_as<IDXGIDevice1>())device1->SetMaximumFrameLatency(1);
        winrt::check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get()));
        timers.create(d3d.get());
        lines.create(d3d.get());d3dc=lines.d3dc;
        pixel_width=w;pixel_height=h;
    }
    void build_effects(){
        const auto layer=[&](winrt::com_ptr<ID2D1Effect>& spread,winrt::com_ptr<ID2D1Effect>& strength,float deviation,float gain){
            winrt::check_hresult(context->CreateEffect(CLSID_D2D1GaussianBlur,spread.put()));winrt::check_hresult(spread->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,deviation));
            winrt::check_hresult(spread->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_SOFT));
            winrt::check_hresult(context->CreateEffect(CLSID_D2D1ColorMatrix,strength.put()));D2D1_MATRIX_5X4_F matrix{};matrix._11=matrix._22=matrix._33=matrix._44=gain;
            winrt::check_hresult(strength->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,matrix));winrt::check_hresult(strength->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT,TRUE));strength->SetInputEffect(0,spread.get());};
        layer(halo,halo_gain,kHaloDeviation,kHaloGain);layer(blur,normalise,kBloomDeviation,kBloomGain*bloom_scale);
        winrt::check_hresult(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED));
        winrt::check_hresult(context->CreateEffect(CLSID_D2D1Composite,glow.put()));winrt::check_hresult(glow->SetValue(D2D1_COMPOSITE_PROP_MODE,D2D1_COMPOSITE_MODE_PLUS));
        glow->SetInputEffect(0,halo_gain.get());glow->SetInputEffect(1,normalise.get());
        winrt::check_hresult(context->CreateEffect(CLSID_D2D1ColorMatrix,tint.put()));tint->SetInputEffect(0,glow.get());
        winrt::check_hresult(tint->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT,TRUE));
        winrt::check_hresult(context->CreateEffect(CLSID_D2D1GaussianBlur,frost_blur.put()));winrt::check_hresult(frost_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,kFrostDeviation));
        winrt::check_hresult(frost_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_SOFT));winrt::check_hresult(frost_blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED));
    }
    void draw_once(dusk::WaveFrame const& frame){
        const auto started=std::chrono::steady_clock::now();
        scale_x=panel.CompositionScaleX();scale_y=panel.CompositionScaleY();
        // The chain covers the whole page (see Dusk.xaml); the waves are drawn in its top part, the stage.
        const double page_width=std::max(frame.width,panel.ActualWidth()),page_height=std::max(frame.height,panel.ActualHeight());
        const unsigned w=std::max(1u,static_cast<unsigned>(std::ceil(page_width*scale_x))),h=std::max(1u,static_cast<unsigned>(std::ceil(page_height*scale_y)));
        if(!chain)create(w,h);
        else if(w!=pixel_width||h!=pixel_height){
            context->SetTarget(nullptr);target=nullptr;glow_in=nullptr;glow_out=nullptr;back_view=nullptr;glow_view=nullptr;glow_texture=nullptr;frost_texture=nullptr;frost_view=nullptr;frost_in=nullptr;frost_out=nullptr;d3dc->ClearState();d3dc->Flush();
            winrt::check_hresult(chain->ResizeBuffers(2,w,h,DXGI_FORMAT_UNKNOWN,0));pixel_width=w;pixel_height=h;
        }
        // The panel shows the buffer at its DIP size: undo the composition scale it applies.
        if(auto chain2=chain.try_as<IDXGISwapChain2>()){DXGI_MATRIX_3X2_F inverse{1/scale_x,0,0,1/scale_y,0,0};chain2->SetMatrixTransform(&inverse);}
        const float dpi=96*scale_x;
        if(!target){
            winrt::com_ptr<IDXGISurface> buffer;winrt::check_hresult(chain->GetBuffer(0,__uuidof(IDXGISurface),buffer.put_void()));
            const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),dpi,dpi);
            winrt::check_hresult(context->CreateBitmapFromDxgiSurface(buffer.get(),&props,target.put()));
            winrt::check_hresult(d3d->CreateRenderTargetView(buffer.as<ID3D11Texture2D>().get(),nullptr,back_view.put()));
        }
        if(!tint)build_effects();
        const float pad=dusk::WaveFrame::padding,glow_dpi=dpi*kGlowResolution;
        const bool glowing=glow_colour.a>0;
        if(glowing&&!glow_in){
            const D2D1_SIZE_U size{static_cast<unsigned>(std::ceil((frame.width+2*pad)*scale_x*kGlowResolution)),static_cast<unsigned>(std::ceil((frame.height+2*pad)*scale_y*kGlowResolution))};
            const auto props=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),glow_dpi,glow_dpi);
            // The glow's input is a Direct3D texture, so the line shader can draw into it and Direct2D's
            // blurs can read it.
            D3D11_TEXTURE2D_DESC desc{size.width,size.height,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
            winrt::check_hresult(d3d->CreateTexture2D(&desc,nullptr,glow_texture.put()));winrt::check_hresult(d3d->CreateRenderTargetView(glow_texture.get(),nullptr,glow_view.put()));
            const auto input=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),glow_dpi,glow_dpi);
            winrt::check_hresult(context->CreateBitmapFromDxgiSurface(glow_texture.as<IDXGISurface>().get(),&input,glow_in.put()));
            winrt::check_hresult(context->CreateBitmap(size,nullptr,0,&props,glow_out.put()));
        }
        const bool frosted=frost>.001f,sharp=frost<.999f;
        const float frost_dpi=dpi*kFrostResolution;
        if(frosted&&!frost_in){
            D3D11_TEXTURE2D_DESC desc{std::max(1u,static_cast<unsigned>(std::ceil(frame.width*scale_x*kFrostResolution))),std::max(1u,static_cast<unsigned>(std::ceil(frame.height*scale_y*kFrostResolution))),1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
            winrt::check_hresult(d3d->CreateTexture2D(&desc,nullptr,frost_texture.put()));winrt::check_hresult(d3d->CreateRenderTargetView(frost_texture.get(),nullptr,frost_view.put()));
            const auto input=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),frost_dpi,frost_dpi);
            winrt::check_hresult(context->CreateBitmapFromDxgiSurface(frost_texture.as<IDXGISurface>().get(),&input,frost_in.put()));
            const auto out=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),frost_dpi,frost_dpi);
            winrt::check_hresult(context->CreateBitmap({desc.Width,desc.Height},nullptr,0,&out,frost_out.put()));
        }
        const auto ink_colour=D2D1::ColorF(frame.ink);
        if(frosted){
            // The lines at a quarter of the resolution, for the frost.
            const float zero[4]{};d3dc->ClearRenderTargetView(frost_view.get(),zero);const float s=scale_x*kFrostResolution;
            for(int i=0;i<frame.count;++i){const float a=frame.alpha[static_cast<size_t>(i)];
                lines.add_line(frame.lines[static_cast<size_t>(i)],0,0,s,std::max(.5f,frame.stroke[static_cast<size_t>(i)]/2*s),ink_colour.r*a,ink_colour.g*a,ink_colour.b*a,a,frame.soft[static_cast<size_t>(i)]*s);}
            D3D11_TEXTURE2D_DESC desc{};frost_texture->GetDesc(&desc);lines.flush(frost_view.get(),static_cast<float>(desc.Width),static_cast<float>(desc.Height));
        }
        timers.begin();
        std::array<int,dusk::WaveFrame::kLines> by_weight{};for(int i=0;i<frame.count;++i)by_weight[i]=i;
        std::stable_sort(by_weight.begin(),by_weight.begin()+frame.count,[&](int a,int b){return frame.weight[a]<frame.weight[b];});
        if(glowing){
            // 1. The lines' union in white at half resolution, with blur padding all round. Fainter
            //    lines first: white source-over never passes full white, so crossings cannot flare.
            const float zero[4]{};d3dc->ClearRenderTargetView(glow_view.get(),zero);
            const float glow_scale=scale_x*kGlowResolution;
            for(int n=0;n<frame.count;++n){const int i=by_weight[n];const float wgt=frame.weight[static_cast<size_t>(i)];lines.add_line(frame.lines[static_cast<size_t>(i)],pad,pad,glow_scale,kStroke/2*glow_scale,wgt,wgt,wgt,wgt);}
            D3D11_TEXTURE2D_DESC desc{};glow_texture->GetDesc(&desc);lines.flush(glow_view.get(),static_cast<float>(desc.Width),static_cast<float>(desc.Height));
            timers.stamp(1);
        }
        context->BeginDraw();
        if(glowing){
            // 2. Halo plus bloom, tinted with the music's colour, still at half resolution.
            // The colour matrix works on straight (not premultiplied) colour: the glow is white, so its
            // colour becomes the music's colour outright and only its alpha is scaled. (0.5.14 scaled
            // the colour by alpha too, which squared the alpha and left the glow almost invisible.)
            D2D1_MATRIX_5X4_F colour{};colour._51=glow_colour.r;colour._52=glow_colour.g;colour._53=glow_colour.b;colour._44=glow_colour.a;
            winrt::check_hresult(tint->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,colour));
            blur->SetInput(0,glow_in.get());halo->SetInput(0,glow_in.get());
            context->SetTarget(glow_out.get());context->SetDpi(glow_dpi,glow_dpi);context->SetTransform(D2D1::Matrix3x2F::Identity());context->Clear(D2D1::ColorF(0,0));context->DrawImage(tint.get());
            blur->SetInput(0,nullptr);halo->SetInput(0,nullptr);
        }else timers.stamp(1);
        if(frosted){
            frost_blur->SetInput(0,frost_in.get());
            context->SetTarget(frost_out.get());context->SetDpi(frost_dpi,frost_dpi);context->SetTransform(D2D1::Matrix3x2F::Identity());context->Clear(D2D1::ColorF(0,0));context->DrawImage(frost_blur.get());
            frost_blur->SetInput(0,nullptr);
        }
        // 3. The stage: black, the glow scaled up, then the crisp lines at full resolution.
        //    Frosting crossfades the sharp lines into their blurred copy.
        context->SetTarget(target.get());context->SetDpi(dpi,dpi);context->SetTransform(D2D1::Matrix3x2F::Identity());context->Clear(D2D1::ColorF(0,0,0,1));
        if(glowing){const auto size=glow_out->GetSize();context->DrawBitmap(glow_out.get(),D2D1::RectF(-pad,-pad,size.width-pad,size.height-pad),1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);}
        if(frosted){const auto size=frost_out->GetSize();context->DrawBitmap(frost_out.get(),D2D1::RectF(0,0,size.width,size.height),frost,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);}
        if(glowing||frosted){
            if(!fade_down){
                const D2D1_GRADIENT_STOP stops[]{{0,D2D1::ColorF(0,0,0,0)},{.55f,D2D1::ColorF(0,0,0,.6f)},{1,D2D1::ColorF(0,0,0,1)}};winrt::com_ptr<ID2D1GradientStopCollection> collection;
                winrt::check_hresult(context->CreateGradientStopCollection(stops,3,collection.put()));
                winrt::check_hresult(context->CreateLinearGradientBrush({},collection.get(),fade_down.put()));winrt::check_hresult(context->CreateLinearGradientBrush({},collection.get(),fade_up.put()));
            }
            const float fw=static_cast<float>(frame.width),fh=static_cast<float>(frame.height),bottom=std::min(kBottomFade,fh*.25f),top=std::max(1.f,frame.fade);
            fade_down->SetStartPoint({0,fh-bottom});fade_down->SetEndPoint({0,fh});context->FillRectangle(D2D1::RectF(0,fh-bottom,fw,fh),fade_down.get());
            fade_up->SetStartPoint({0,top});fade_up->SetEndPoint({0,0});context->FillRectangle(D2D1::RectF(0,0,fw,top),fade_up.get());
        }
        winrt::check_hresult(context->EndDraw());timers.stamp(2);
        {
            // 4. The crisp lines, by the line shader, straight into the frame.
            if(sharp)for(int i=0;i<frame.count;++i){const float a=frame.alpha[static_cast<size_t>(i)]*(1-frost);
                lines.add_line(frame.lines[static_cast<size_t>(i)],0,0,scale_x,frame.stroke[static_cast<size_t>(i)]/2*scale_x,ink_colour.r*a,ink_colour.g*a,ink_colour.b*a,a,frame.soft[static_cast<size_t>(i)]*scale_x);}
            lines.flush(back_view.get(),static_cast<float>(pixel_width),static_cast<float>(pixel_height),frame.fade*scale_x,static_cast<float>(frame.height)*scale_y);
        }
        lines.dither(back_view.get(),static_cast<float>(pixel_width),static_cast<float>(pixel_height));
        timers.stamp(3);
        if(!capture.empty())save_capture();
        winrt::check_hresult(chain->Present(1,0));
        timers.end(glowing,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
        sequence=frame.sequence;++presents;
    }
    void draw(dusk::WaveFrame const& frame){
        if(!panel||!frame.valid_after(sequence))return;
        if(panel.CompositionScaleX()<=0)return;
        try{draw_once(frame);}catch(winrt::hresult_error const& error){
            const auto code=error.code();
            if(code!=DXGI_ERROR_DEVICE_REMOVED&&code!=DXGI_ERROR_DEVICE_RESET&&code!=D2DERR_RECREATE_TARGET)throw;
            reset();draw_once(frame); // a lost device: start again with a new one
        }
    }
    void set_colour(winrt::Windows::UI::Color value){glow_colour={value.R/255.f,value.G/255.f,value.B/255.f,value.A/255.f};}
};
