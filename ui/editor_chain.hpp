#pragma once
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <microsoft.ui.xaml.media.dxinterop.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <array>
#include <cmath>
#include <vector>
#include "line_pipeline.hpp"
#include "audio_analysis.hpp"

// The EQ editor's graph background (0.5.18), in a swap chain behind the curve and its handles:
//  - a live spectrum of what is playing, a soft glowing area in the music's colour, laid out on the
//    same pitch scale as the five bands, so you can see what each band is acting on;
//  - the EQ curve filled with a gradient towards the 0 dB line, so boosts and cuts read at a glance;
//  - while Compare is held, a dotted afterimage of how the EQ was.
// The curve itself, its glow and the handles stay XAML on top. The plot card behind is 2% white on
// black, and the chain sits inside its rounded corners, so it is opaque in that colour.
struct EditorChain {
    struct Scene {
        float width=0,height=0;                    // the panel, in DIPs
        std::vector<dusk::WavePoint> curve,ghost;   // in panel DIPs; ghost is empty unless comparing
        float ghost_alpha=0;
        float zero_y=0,floor_y=0,top_y=0,left=0,right=0;
        std::array<float,dusk::kFine> spectrum{};   // 0..1, already smoothed
        std::array<float,dusk::kFine> spectrum_x{}; // where each band sits, in panel DIPs
        D2D1_COLOR_F colour{1,1,1,1};float glow=0;  // the music's colour, and how much music there is
    };
    winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel panel{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;winrt::com_ptr<IDXGISwapChain1> chain;winrt::com_ptr<ID3D11RenderTargetView> back_view;
    LinePipeline lines;unsigned pixel_width=0,pixel_height=0;
    static constexpr float kBackground=5/255.f;
    std::filesystem::path capture; // development (--stage-bench)

    void attach(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& value){panel=value;}
    void reset(){back_view=nullptr;lines=LinePipeline{};chain=nullptr;d3d=nullptr;pixel_width=pixel_height=0;}
    void create(unsigned w,unsigned h){
        if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d3d.put(),nullptr,nullptr)))
            winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d3d.put(),nullptr,nullptr));
        auto dxgi=d3d.as<IDXGIDevice>();winrt::com_ptr<IDXGIAdapter> adapter;winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2),factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=w;desc.Height=h;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount=2;desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;
        winrt::check_hresult(factory->CreateSwapChainForComposition(d3d.get(),&desc,nullptr,chain.put()));
        if(auto device1=d3d.try_as<IDXGIDevice1>())device1->SetMaximumFrameLatency(1);
        winrt::check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get()));
        lines.create(d3d.get());pixel_width=w;pixel_height=h;
    }
    static D2D1_COLOR_F premultiplied(D2D1_COLOR_F c,float a){return {c.r*a,c.g*a,c.b*a,a};}
    void draw_once(Scene const& scene){
        const float sx=panel.CompositionScaleX(),sy=panel.CompositionScaleY();
        const unsigned w=std::max(1u,static_cast<unsigned>(std::ceil(scene.width*sx))),h=std::max(1u,static_cast<unsigned>(std::ceil(scene.height*sy)));
        if(!chain)create(w,h);
        else if(w!=pixel_width||h!=pixel_height){back_view=nullptr;lines.d3dc->ClearState();lines.d3dc->Flush();winrt::check_hresult(chain->ResizeBuffers(2,w,h,DXGI_FORMAT_UNKNOWN,0));pixel_width=w;pixel_height=h;}
        if(auto chain2=chain.try_as<IDXGISwapChain2>()){DXGI_MATRIX_3X2_F inverse{1/sx,0,0,1/sy,0,0};chain2->SetMatrixTransform(&inverse);}
        if(!back_view){winrt::com_ptr<ID3D11Texture2D> buffer;winrt::check_hresult(chain->GetBuffer(0,__uuidof(ID3D11Texture2D),buffer.put_void()));winrt::check_hresult(d3d->CreateRenderTargetView(buffer.get(),nullptr,back_view.put()));}
        const float background[4]{kBackground,kBackground,kBackground,1};lines.d3dc->ClearRenderTargetView(back_view.get(),background);
        // 1. The spectrum: a soft area rising from the floor, and a soft line along its top.
        if(scene.glow>0){
            std::vector<dusk::WavePoint> knots,top;std::vector<float> floor;
            const float span=scene.floor_y-scene.top_y;
            const auto level_y=[&](float level){return scene.floor_y-std::clamp(level,0.f,1.f)*span*.6f;};
            for(size_t i=0;i<dusk::kFine;++i)if(scene.spectrum_x[i]>=scene.left&&scene.spectrum_x[i]<=scene.right)knots.push_back({scene.spectrum_x[i],level_y(scene.spectrum[i])});
            if(knots.size()<2)knots={{scene.left,scene.floor_y},{scene.right,scene.floor_y}};
            knots.insert(knots.begin(),{scene.left,knots.front().y});
            // A smooth outline through the bands (Catmull-Rom, eight steps a band): straight segments
            // looked jagged, and their wide quads showed faint stripes in the gradient.
            for(size_t k=0;k+1<knots.size();++k){
                const auto& p0=knots[k?k-1:k];const auto& p1=knots[k];const auto& p2=knots[k+1];const auto& p3=knots[std::min(k+2,knots.size()-1)];
                for(int step=0;step<8;++step){const float t=step/8.f,t2=t*t,t3=t2*t;
                    const float x=p1.x+(p2.x-p1.x)*t;
                    const float y=.5f*((2*p1.y)+(-p0.y+p2.y)*t+(2*p0.y-5*p1.y+4*p2.y-p3.y)*t2+(-p0.y+3*p1.y-3*p2.y+p3.y)*t3);
                    top.push_back({x,std::clamp(y,scene.top_y,scene.floor_y)});}
            }
            top.push_back(knots.back());
            top.push_back({scene.right,top.back().y});
            floor.assign(top.size(),scene.floor_y);
            lines.add_fill(top,floor,sx,premultiplied(scene.colour,.13f*scene.glow),premultiplied(scene.colour,.01f*scene.glow));
            // A narrow soft edge: a wide one folded over itself at sharp peaks and showed as creases.
            lines.add_line(top,0,0,sx,.6f*sx,scene.colour.r*.3f*scene.glow,scene.colour.g*.3f*scene.glow,scene.colour.b*.3f*scene.glow,.3f*scene.glow,1.5f*sx);
        }
        // 2. The EQ curve's fill, strongest at the curve and fading towards 0 dB. In silence it is
        //    white and faint; with music it takes the music's colour.
        if(scene.curve.size()>1){
            const D2D1_COLOR_F tint{1+(scene.colour.r-1)*scene.glow,1+(scene.colour.g-1)*scene.glow,1+(scene.colour.b-1)*scene.glow,1};
            std::vector<float> zero(scene.curve.size(),scene.zero_y);
            lines.add_fill(scene.curve,zero,sx,premultiplied(tint,.10f+.10f*scene.glow),premultiplied(tint,0));
        }
        // 3. The curve's glow, under the XAML stroke: a wide soft halo and a tighter one, only with music.
        //    (It was a separate Direct2D glow until 0.5.18; drawn here it gets the dithering too.)
        if(scene.curve.size()>1&&scene.glow>0){
            const auto& c=scene.colour;const float wide=.16f*scene.glow,tight=.38f*scene.glow;
            lines.add_line(scene.curve,0,0,sx,1*sx,c.r*wide,c.g*wide,c.b*wide,wide,22*sx);
            lines.add_line(scene.curve,0,0,sx,1*sx,c.r*tight,c.g*tight,c.b*tight,tight,6*sx);
        }
        // 4. Compare: how the EQ was, as a dashed afterimage.
        if(scene.ghost.size()>1&&scene.ghost_alpha>0){const float a=.6f*scene.ghost_alpha;lines.add_dots(scene.ghost,sx,.8f*sx,.95f*a,.95f*a,.95f*a,a,12,7);}
        lines.flush(back_view.get(),static_cast<float>(pixel_width),static_cast<float>(pixel_height));
        lines.dither(back_view.get(),static_cast<float>(pixel_width),static_cast<float>(pixel_height));
        if(!capture.empty()){save_chain_capture(chain.get(),d3d.get(),lines.d3dc.get(),capture);capture.clear();}
        winrt::check_hresult(chain->Present(1,0));
    }
    void draw(Scene const& scene){
        if(!panel||scene.width<2||scene.height<2||panel.CompositionScaleX()<=0)return;
        try{draw_once(scene);}catch(winrt::hresult_error const& error){
            if(error.code()!=DXGI_ERROR_DEVICE_REMOVED&&error.code()!=DXGI_ERROR_DEVICE_RESET)throw;
            reset();draw_once(scene);
        }
    }
};
