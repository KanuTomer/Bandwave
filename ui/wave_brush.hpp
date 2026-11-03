#pragma once
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Composition.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.Graphics.DirectX.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include "wave_frame.hpp"

// Retained GPU surface: every draw consumes the same animated frame as the
// visible strokes. No PNG encoding, image loading or CPU pixel mask.
struct WaveBrush : winrt::Microsoft::UI::Xaml::Media::XamlCompositionBrushBaseT<WaveBrush> {
    winrt::Microsoft::UI::Composition::Compositor compositor{nullptr};
    winrt::Microsoft::UI::Composition::CompositionColorBrush tint{nullptr};
    winrt::Microsoft::UI::Composition::CompositionMaskBrush mask{nullptr};
    winrt::Microsoft::UI::Composition::CompositionGraphicsDevice graphics{nullptr};
    winrt::Microsoft::UI::Composition::CompositionDrawingSurface surface{nullptr};
    winrt::Microsoft::UI::Composition::CompositionSurfaceBrush source{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID2D1Factory1> factory;
    winrt::com_ptr<ID2D1Device> device;
    winrt::com_ptr<ID2D1Effect> blur,normalise,halo,halo_gain,glow;
    winrt::com_ptr<ID2D1SolidColorBrush> white;
    // A thin core, a tight halo that stays with its own line, and a faint wide bloom.
    static constexpr float kStroke=3,kHaloDeviation=5,kHaloGain=3.4f,kBloomDeviation=24,kBloomGain=1.6f;
    // The glow is drawn at half the screen's resolution and scaled up. It is only blur, so it looks
    // the same, and both blurs touch a quarter of the pixels: they were most of the app's GPU time
    // (about 44% of the 3D engine with music playing, 4 Nov 2026). Sizes stay in DIPs, so the
    // halo and bloom keep their width.
    static constexpr float kResolution=.5f;
    unsigned pixel_width=0,pixel_height=0,allocations=0;
    uint64_t sequence=0;
    bool warp=false;
    void initialise(winrt::Microsoft::UI::Composition::Compositor const& value){
        compositor=value;
        if(!tint)tint=compositor.CreateColorBrush();
        if(!mask){mask=compositor.CreateMaskBrush();mask.Source(tint);CompositionBrush(mask);}
        auto result=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d3d.put(),nullptr,nullptr);
        if(FAILED(result)){warp=true;winrt::check_hresult(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,d3d.put(),nullptr,nullptr));}
        D2D1_FACTORY_OPTIONS options{};
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),&options,factory.put_void()));
        auto dxgi=d3d.as<IDXGIDevice>();winrt::check_hresult(factory->CreateDevice(dxgi.get(),device.put()));
        auto interop=compositor.as<winrt::Microsoft::UI::Composition::ICompositorInterop>();
        winrt::Microsoft::UI::Composition::ICompositionGraphicsDevice projected{nullptr};winrt::check_hresult(interop->CreateGraphicsDevice(device.get(),&projected));graphics=projected.as<winrt::Microsoft::UI::Composition::CompositionGraphicsDevice>();
    }
    void draw_once(dusk::WaveFrame const& frame){
        using namespace winrt::Microsoft::UI::Composition;
        const float dpi=static_cast<float>(frame.dpi)*kResolution,pad=dusk::WaveFrame::padding;
        const unsigned w=static_cast<unsigned>(std::ceil((frame.width+2*pad)*dpi)),h=static_cast<unsigned>(std::ceil((frame.height+2*pad)*dpi));
        if(!surface||w!=pixel_width||h!=pixel_height){
            surface=graphics.CreateDrawingSurface({static_cast<float>(w),static_cast<float>(h)},winrt::Microsoft::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,winrt::Microsoft::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            source=compositor.CreateSurfaceBrush(surface);source.Stretch(CompositionStretch::None);source.HorizontalAlignmentRatio(0);source.VerticalAlignmentRatio(0);source.Scale({1/dpi,1/dpi});source.Offset({-pad,-pad});mask.Mask(source);
            pixel_width=w;pixel_height=h;++allocations;blur=nullptr;normalise=nullptr;halo=nullptr;halo_gain=nullptr;glow=nullptr;white=nullptr;
        }
        auto interop=surface.as<ICompositionDrawingSurfaceInterop>();winrt::com_ptr<ID2D1DeviceContext> context;POINT offset{};
        winrt::check_hresult(interop->BeginDraw(nullptr,__uuidof(ID2D1DeviceContext),context.put_void(),&offset));
        try{
            context->SetDpi(96*dpi,96*dpi);
            winrt::com_ptr<ID2D1Image> target;context->GetTarget(target.put());
            if(!white)winrt::check_hresult(context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),white.put()));
            if(!blur){
                const auto layer=[&](winrt::com_ptr<ID2D1Effect>& spread,winrt::com_ptr<ID2D1Effect>& strength,float deviation,float gain){
                    winrt::check_hresult(context->CreateEffect(CLSID_D2D1GaussianBlur,spread.put()));winrt::check_hresult(spread->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,deviation));
                    winrt::check_hresult(context->CreateEffect(CLSID_D2D1ColorMatrix,strength.put()));D2D1_MATRIX_5X4_F matrix{};matrix._11=matrix._22=matrix._33=matrix._44=gain;
                    winrt::check_hresult(strength->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,matrix));winrt::check_hresult(strength->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT,TRUE));strength->SetInputEffect(0,spread.get());};
                layer(halo,halo_gain,kHaloDeviation,kHaloGain);layer(blur,normalise,kBloomDeviation,kBloomGain);
                // The wide bloom has no detail to keep, so Direct2D may shrink it further while blurring.
                winrt::check_hresult(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED));
                for(auto* effect:{blur.get(),halo.get()})winrt::check_hresult(effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_SOFT));
                winrt::check_hresult(context->CreateEffect(CLSID_D2D1Composite,glow.put()));winrt::check_hresult(glow->SetValue(D2D1_COMPOSITE_PROP_MODE,D2D1_COMPOSITE_MODE_PLUS));
                glow->SetInputEffect(0,halo_gain.get());glow->SetInputEffect(1,normalise.get());
            }
            winrt::com_ptr<ID2D1CommandList> commands;winrt::check_hresult(context->CreateCommandList(commands.put()));context->SetTarget(commands.get());context->SetTransform(D2D1::Matrix3x2F::Identity());context->Clear(D2D1::ColorF(0,0));
            // Opaque source-over strokes form a union, so coincident Off lines
            // cannot add brightness. Both blurs are GPU effects over that union.
            for(auto const& points:frame.lines){if(points.empty())continue;winrt::com_ptr<ID2D1PathGeometry> path;winrt::check_hresult(factory->CreatePathGeometry(path.put()));winrt::com_ptr<ID2D1GeometrySink> sink;winrt::check_hresult(path->Open(sink.put()));
                sink->BeginFigure({points[0].x+pad,points[0].y+pad},D2D1_FIGURE_BEGIN_HOLLOW);
                for(size_t i=1;i<points.size();++i)sink->AddLine({points[i].x+pad,points[i].y+pad});sink->EndFigure(D2D1_FIGURE_END_OPEN);winrt::check_hresult(sink->Close());context->DrawGeometry(path.get(),white.get(),kStroke);
            }
            context->SetTarget(target.get());winrt::check_hresult(commands->Close());blur->SetInput(0,commands.get());halo->SetInput(0,commands.get());
            context->SetTransform(D2D1::Matrix3x2F::Translation(offset.x/dpi,offset.y/dpi));context->Clear(D2D1::ColorF(0,0));context->DrawImage(glow.get());
            winrt::check_hresult(interop->EndDraw());blur->SetInput(0,nullptr);halo->SetInput(0,nullptr);sequence=frame.sequence;
        }catch(...){interop->EndDraw();throw;}
    }
    void draw(dusk::WaveFrame const& frame){
        if(!frame.valid_after(sequence))return;
        if(!frame.covers_edges())throw std::runtime_error("Wave frame lacks blur padding.");
        try{draw_once(frame);}catch(winrt::hresult_error const& error){
            if(error.code()!=DXGI_ERROR_DEVICE_REMOVED&&error.code()!=DXGI_ERROR_DEVICE_RESET&&error.code()!=D2DERR_RECREATE_TARGET)throw;
            surface=nullptr;graphics=nullptr;device=nullptr;d3d=nullptr;factory=nullptr;initialise(compositor);draw_once(frame);
        }
    }
    unsigned previous=0;bool coloured=false;
    void set_colour(winrt::Windows::UI::Color value,bool animate=false){
        unsigned packed=(unsigned(value.A)<<24)|(unsigned(value.R)<<16)|(unsigned(value.G)<<8)|value.B;if(coloured&&packed==previous)return;
        if(animate&&coloured){auto transition=compositor.CreateColorKeyFrameAnimation();transition.InsertExpressionKeyFrame(0,L"this.StartingValue");transition.InsertKeyFrame(1,value);transition.Duration(std::chrono::milliseconds(120));tint.StartAnimation(L"Color",transition);}else{tint.StopAnimation(L"Color");tint.Color(value);}
        previous=packed;coloured=true;
    }
    void OnConnected(){if(mask)CompositionBrush(mask);}
    void OnDisconnected(){}
};
