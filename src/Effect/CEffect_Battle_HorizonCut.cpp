#include "Effect/CEffect_Battle_HorizonCut.h"
#include "Effect/CEAManager.h"
#include "global.h" // 假設用於獲取全域變數 g_Game_System_Info

// 假設的全域變數
extern GameSystemInfo g_Game_System_Info;

// 對應反組譯碼: 0x0052EA80
CEffect_Battle_HorizonCut::CEffect_Battle_HorizonCut()
{
    // CEffectBase 的建構函式會被自動呼叫

    // 向 CEAManager 請求特效數據
    // 特效 ID: 1, 檔案名稱: "MoFData/Effect/efn_horizoncut.ea"
    CEAManager::GetInstance()->GetEAData(1, "MoFData/Effect/efn_horizoncut.ea", &m_ccaEffect);

    // 設定影格時間並播放動畫
    m_ccaEffect.SetFrameTime();
    m_ccaEffect.Play(0, false); // 播放第一個動畫序列，不循環
}

// 對應反組譯碼: 0x0052EB10
CEffect_Battle_HorizonCut::~CEffect_Battle_HorizonCut()
{
}

// 對應反組譯碼: 0x0052EB20
void CEffect_Battle_HorizonCut::SetEffect(float x, float y, bool bFlip, unsigned char a5)
{
    // 設定特效的初始世界座標
    // 原始碼: *((float *)this + 2) = a2; *((float *)this + 3) = a3;
    m_fCurrentPosX = x;
    m_fCurrentPosY = y;

    // 設定特效是否需要翻轉
    // 原始碼: *((_BYTE *)this + 56) = a4;
    m_bIsFlip = bFlip;
}

// 對應反組譯碼: 0x0052EB40
bool CEffect_Battle_HorizonCut::FrameProcess(float fElapsedTime)
{
    // 將生命週期管理的任務完全委託給內部的 CCAEffect 物件。
    // 當 "efn_horizoncut.ea" 動畫播放完畢時，此函式會回傳 true。
    return m_ccaEffect.FrameProcess(fElapsedTime);
}

// 對應反組譯碼: 0x0052EB50
void CEffect_Battle_HorizonCut::Process()
{
    // 將世界座標轉換為螢幕座標
    // 原始碼: v3 = *((float *)this + 2) - (double)dword_A73088;
    float screenX = m_fCurrentPosX - static_cast<float>(g_Game_System_Info.ScreenWidth);
    float screenY = m_fCurrentPosY - static_cast<float>(g_Game_System_Info.ScreenHeight);

    // 進行裁剪判斷
    // 原始碼: v2 = CEffectBase::IsCliping(this, v3, 0.0); *((_DWORD *)this + 8) = v2;
    m_bIsVisible = IsCliping(screenX, 0.0f);

    if (m_bIsVisible) {
        // 更新內部 CCAEffect 的狀態
        // 原始碼: *((float *)this + 12) = ...; *((float *)this + 13) = ...;
        m_ccaEffect.SetPosition(screenX, screenY);
        m_ccaEffect.SetFlipX(m_bIsFlip);
        m_ccaEffect.Process();
    }
}

// 對應反組譯碼: 0x0052EB90
void CEffect_Battle_HorizonCut::Draw()
{
    if (m_bIsVisible) {
        m_ccaEffect.Draw();
    }
}