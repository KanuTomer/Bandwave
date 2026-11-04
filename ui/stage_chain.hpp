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
    winrt::com_ptr<ID2D1Effect> blur,normalise,halo,halo_gain,glow,tint;
    static constexpr float kStroke=3,kHaloDeviation=5,kHaloGain=3.4f,kBloomDeviation=24,kBloomGain=1.6f;
    static constexpr float kGlowResolution=.5f; // the glow is only blur, so half resolution looks the same
    float bloom_scale=1;
    unsigned pixel_width=0,pixel_height=0;
    float scale_x=1,scale_y=1;
    D2D1_COLOR_F glow_colour{0,0,0,0};
    uint64_t sequence=0,presents=0;
    StageTimers timers;
    // ---- The line shader (0.5.17) ----
    // Direct2D's antialiased strokes of long curved lines were most of the stage's GPU time: about
    // half for the crisp lines and a quarter for the glow strokes at 2560x1600 (measured with parts
    // switched off, 4 Nov 2026). Each line is now a strip of quads, two vertices per point, offset
    // along the line's normal, all drawn by the GPU in one call. The pixel shader works out each
    // pixel's coverage from its distance across the line, so edges stay smooth. Neighbouring quads
    // share their edge exactly, so a translucent line never doubles up at its joins.
    struct LineVertex{float x,y,across,half,r,g,b,a;};
    winrt::com_ptr<ID3D11DeviceContext> d3dc;
    winrt::com_ptr<ID3D11VertexShader> line_vs;winrt::com_ptr<ID3D11PixelShader> line_ps;winrt::com_ptr<ID3D11InputLayout> line_layout;
    winrt::com_ptr<ID3D11Buffer> line_buffer,line_constants;winrt::com_ptr<ID3D11BlendState> line_blend;winrt::com_ptr<ID3D11RasterizerState> line_raster;
    winrt::com_ptr<ID3D11RenderTargetView> back_view,glow_view;winrt::com_ptr<ID3D11Texture2D> glow_texture;
    UINT line_capacity=0;std::vector<LineVertex> line_vertices;
    void create_line_pipeline(){
        static constexpr char source[]=
            "cbuffer Target:register(b0){float2 size;float2 unused;};\n"
            "struct In{float2 p:POSITION;float across:ACROSS;float half:HALF;float4 c:COLOR;};\n"
            "struct Out{float4 p:SV_Position;float across:ACROSS;float half:HALF;float4 c:COLOR;};\n"
            "Out vs(In v){Out o;o.p=float4(v.p.x/size.x*2-1,1-v.p.y/size.y*2,0,1);o.across=v.across;o.half=v.half;o.c=v.c;return o;}\n"
            "float4 ps(Out i):SV_Target{return i.c*saturate(i.half+.5-abs(i.across));}\n";
        const auto compile=[&](const char* entry,const char* profile){winrt::com_ptr<ID3DBlob> code,errors;
            if(FAILED(D3DCompile(source,sizeof(source)-1,"stage_lines",nullptr,nullptr,entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,code.put(),errors.put())))
                throw std::runtime_error(errors?static_cast<const char*>(errors->GetBufferPointer()):"Line shader did not compile.");
            return code;};
        auto vcode=compile("vs","vs_4_0"),pcode=compile("ps","ps_4_0");
        winrt::check_hresult(d3d->CreateVertexShader(vcode->GetBufferPointer(),vcode->GetBufferSize(),nullptr,line_vs.put()));
        winrt::check_hresult(d3d->CreatePixelShader(pcode->GetBufferPointer(),pcode->GetBufferSize(),nullptr,line_ps.put()));
        const D3D11_INPUT_ELEMENT_DESC layout[]{{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"ACROSS",0,DXGI_FORMAT_R32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"HALF",0,DXGI_FORMAT_R32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
        winrt::check_hresult(d3d->CreateInputLayout(layout,4,vcode->GetBufferPointer(),vcode->GetBufferSize(),line_layout.put()));
        D3D11_BUFFER_DESC constants{16,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};winrt::check_hresult(d3d->CreateBuffer(&constants,nullptr,line_constants.put()));
        // Premultiplied source-over, the same blending the Polylines and Direct2D strokes had.
        D3D11_BLEND_DESC blend{};auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;winrt::check_hresult(d3d->CreateBlendState(&blend,line_blend.put()));
        D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;winrt::check_hresult(d3d->CreateRasterizerState(&raster,line_raster.put()));
        d3dc=nullptr;d3d->GetImmediateContext(d3dc.put());line_capacity=0;line_buffer=nullptr;
    }
    // One line as a strip of quads: points in DIPs, moved by (dx,dy), scaled to pixels.
    void add_line(std::vector<dusk::WavePoint> const& points,float dx,float dy,float scale,float half,float r,float g,float b,float a){
        if(points.size()<2||a<=0)return;
        const float reach=half+1; // one pixel beyond the edge, for the antialiasing
        const size_t n=points.size();std::vector<std::array<float,4>> side(n);
        for(size_t k=0;k<n;++k){
            const auto& before=points[k?k-1:k];const auto& after=points[k+1<n?k+1:k];
            float tx=after.x-before.x,ty=after.y-before.y;const float length=std::sqrt(tx*tx+ty*ty);if(length>0){tx/=length;ty/=length;}else{tx=1;ty=0;}
            const float x=(points[k].x+dx)*scale,y=(points[k].y+dy)*scale,nx=-ty*reach,ny=tx*reach;
            side[k]={x-nx,y-ny,x+nx,y+ny};
        }
        for(size_t k=0;k+1<n;++k){
            const LineVertex l0{side[k][0],side[k][1],-reach,half,r,g,b,a},r0{side[k][2],side[k][3],reach,half,r,g,b,a},
                             l1{side[k+1][0],side[k+1][1],-reach,half,r,g,b,a},r1{side[k+1][2],side[k+1][3],reach,half,r,g,b,a};
            line_vertices.insert(line_vertices.end(),{l0,r0,l1,r0,r1,l1});
        }
    }
    void flush_lines(ID3D11RenderTargetView* view,float width,float height){
        if(line_vertices.empty())return;
        const UINT count=static_cast<UINT>(line_vertices.size());
        if(count>line_capacity){line_capacity=count+count/2;line_buffer=nullptr;D3D11_BUFFER_DESC desc{static_cast<UINT>(line_capacity*sizeof(LineVertex)),D3D11_USAGE_DYNAMIC,D3D11_BIND_VERTEX_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};winrt::check_hresult(d3d->CreateBuffer(&desc,nullptr,line_buffer.put()));}
        D3D11_MAPPED_SUBRESOURCE mapped{};winrt::check_hresult(d3dc->Map(line_buffer.get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));memcpy(mapped.pData,line_vertices.data(),count*sizeof(LineVertex));d3dc->Unmap(line_buffer.get(),0);
        winrt::check_hresult(d3dc->Map(line_constants.get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));const float size[4]{width,height,0,0};memcpy(mapped.pData,size,sizeof(size));d3dc->Unmap(line_constants.get(),0);
        // Direct2D shares this device and leaves its own state behind: set everything the draw uses.
        const UINT stride=sizeof(LineVertex),offset=0;ID3D11Buffer* buffers[]{line_buffer.get()};ID3D11Buffer* cbs[]{line_constants.get()};
        d3dc->IASetInputLayout(line_layout.get());d3dc->IASetVertexBuffers(0,1,buffers,&stride,&offset);d3dc->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dc->VSSetShader(line_vs.get(),nullptr,0);d3dc->VSSetConstantBuffers(0,1,cbs);d3dc->PSSetShader(line_ps.get(),nullptr,0);
        d3dc->GSSetShader(nullptr,nullptr,0);d3dc->HSSetShader(nullptr,nullptr,0);d3dc->DSSetShader(nullptr,nullptr,0);
        d3dc->RSSetState(line_raster.get());const D3D11_VIEWPORT viewport{0,0,width,height,0,1};d3dc->RSSetViewports(1,&viewport);
        const float factor[4]{};d3dc->OMSetBlendState(line_blend.get(),factor,0xffffffff);d3dc->OMSetDepthStencilState(nullptr,0);
        ID3D11RenderTargetView* views[]{view};d3dc->OMSetRenderTargets(1,views,nullptr);
        d3dc->Draw(count,0);
        ID3D11RenderTargetView* none[]{nullptr};d3dc->OMSetRenderTargets(1,none,nullptr);
        line_vertices.clear();
    }
    void attach(winrt::Microsoft::UI::Xaml::Controls::SwapChainPanel const& value){panel=value;}
    void reset(){
        if(context)context->SetTarget(nullptr);
        target=nullptr;glow_in=nullptr;glow_out=nullptr;blur=normalise=halo=halo_gain=glow=tint=nullptr;
        back_view=nullptr;glow_view=nullptr;glow_texture=nullptr;line_buffer=nullptr;line_constants=nullptr;line_vs=nullptr;line_ps=nullptr;line_layout=nullptr;line_blend=nullptr;line_raster=nullptr;d3dc=nullptr;
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
        create_line_pipeline();
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
    }
    void draw_once(dusk::WaveFrame const& frame){
        const auto started=std::chrono::steady_clock::now();
        scale_x=panel.CompositionScaleX();scale_y=panel.CompositionScaleY();
        const unsigned w=std::max(1u,static_cast<unsigned>(std::ceil(frame.width*scale_x))),h=std::max(1u,static_cast<unsigned>(std::ceil(frame.height*scale_y)));
        if(!chain)create(w,h);
        else if(w!=pixel_width||h!=pixel_height){
            context->SetTarget(nullptr);target=nullptr;glow_in=nullptr;glow_out=nullptr;back_view=nullptr;glow_view=nullptr;glow_texture=nullptr;d3dc->ClearState();d3dc->Flush();
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
        timers.begin();
        std::array<int,dusk::WaveFrame::kLines> by_weight{};for(int i=0;i<frame.count;++i)by_weight[i]=i;
        std::stable_sort(by_weight.begin(),by_weight.begin()+frame.count,[&](int a,int b){return frame.weight[a]<frame.weight[b];});
        if(glowing){
            // 1. The lines' union in white at half resolution, with blur padding all round. Fainter
            //    lines first: white source-over never passes full white, so crossings cannot flare.
            const float zero[4]{};d3dc->ClearRenderTargetView(glow_view.get(),zero);
            const float glow_scale=scale_x*kGlowResolution;
            for(int n=0;n<frame.count;++n){const int i=by_weight[n];const float wgt=frame.weight[static_cast<size_t>(i)];add_line(frame.lines[static_cast<size_t>(i)],pad,pad,glow_scale,kStroke/2*glow_scale,wgt,wgt,wgt,wgt);}
            D3D11_TEXTURE2D_DESC desc{};glow_texture->GetDesc(&desc);flush_lines(glow_view.get(),static_cast<float>(desc.Width),static_cast<float>(desc.Height));
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
        // 3. The stage: black, the glow scaled up, then the crisp lines at full resolution.
        context->SetTarget(target.get());context->SetDpi(dpi,dpi);context->SetTransform(D2D1::Matrix3x2F::Identity());context->Clear(D2D1::ColorF(0,0,0,1));
        if(glowing){const auto size=glow_out->GetSize();context->DrawBitmap(glow_out.get(),D2D1::RectF(-pad,-pad,size.width-pad,size.height-pad),1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);}
        winrt::check_hresult(context->EndDraw());timers.stamp(2);
        {
            // 4. The crisp lines, by the line shader, straight into the frame.
            const auto ink_colour=D2D1::ColorF(frame.ink);
            for(int i=0;i<frame.count;++i){const float a=frame.alpha[static_cast<size_t>(i)];
                add_line(frame.lines[static_cast<size_t>(i)],0,0,scale_x,frame.stroke[static_cast<size_t>(i)]/2*scale_x,ink_colour.r*a,ink_colour.g*a,ink_colour.b*a,a);}
            flush_lines(back_view.get(),static_cast<float>(pixel_width),static_cast<float>(pixel_height));
        }
        timers.stamp(3);
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
