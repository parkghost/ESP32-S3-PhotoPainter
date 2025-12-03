# ESP32-S3-PhotoPainter
中文wiki链接: https://www.waveshare.net/wiki/ESP32-S3-PhotoPainter<br>
Product English wiki link: https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter

---

## 增加功能

| 功能 | 說明 |
|:-----|:-----|
| **Gemini AI 圖像生成** | Google Gemini API 支援，用於 AI 圖像生成 |
| **直接顯示模式** | AI 圖像生成直接輸出到電子紙畫面，跳過 SD 卡寫入/讀取流程，**每張圖像約可節省 40 秒** |
| **抖動演算法與色彩校正** | 基於 [epdoptimize](https://github.com/Utzel-Butzel/epdoptimize) 優化，提供更優質的 6 色電子紙顯示效果 |

---

## 已關閉的功能

| 功能 | 狀態 | 原因 |
|:-----|:----:|:-----|
| 天氣資訊 | 已停用 | 限中國大陸使用 |

---

## 設定說明

### 設定檔位置

```
02_SDCARD/06_user_Foundation_img/config.txt
```

### 完整設定範例

```json
{
  "timer": 1200,
  "ai_provider": "gemini",
  "ai_model": "gemini-3-pro-image-preview",
  "ai_key": "YOUR_GEMINI_API_KEY",
  "ai_direct_display": true,
  "dither_kernel": "jarvis",
  "dither_serpentine": true,
  "dither_color_calibration": {
    "black": [25, 30, 33],
    "white": [232, 232, 232],
    "red": [178, 19, 24],
    "green": [18, 95, 32],
    "blue": [33, 87, 186],
    "yellow": [239, 222, 68]
  }
}
```

### 參數說明

#### 基本設定

| 欄位 | 值 | 說明 |
|:-----|:---|:-----|
| `timer` | `1200` | 圖像更新間隔時間（秒） |
| `ai_provider` | `gemini` | AI 服務供應商（若未指定，將從模型名稱自動偵測） |
| `ai_model` | `gemini-2.5-flash-image`<br>`gemini-3-pro-image-preview` | AI 模型名稱 |
| `ai_key` | `YOUR_GEMINI_API_KEY` | API 金鑰（**必填**） |
| `ai_direct_display` | `true` / `false` | 跳過 SD 卡 I/O 以加快顯示速度 |

**備註**： 兩種 Gemini 模型的圖像生成功能，需要升級為 `Paid Tier` 才可使用，詳見 [Gemini Developer API pricing](https://ai.google.dev/gemini-api/docs/pricing)

#### 抖動演算法設定

| 欄位 | 預設值 | 說明 |
|:-----|:-------|:-----|
| `dither_kernel` | `jarvis` | 抖動演算法選擇 |
| `dither_serpentine` | `true` | 蛇形掃描，減少條紋瑕疵 |
| `dither_color_calibration` | 標準 RGB | 6 色校準值，用於抖動時的色彩匹配 |

<details>
<summary><strong>可用的抖動演算法</strong></summary>

| 演算法 | 品質 | 速度 |
|:-------|:----:|:----:|
| `jarvis` | 最佳 | 較慢 |
| `stucki` | 優良 | 中等 |
| `sierra` | 良好 | 較快 |
| `floyd_steinberg` | 標準 | 最快 |

</details>

---

## MCP 工具整合

### aiIMG 工具說明 (xiaozhi.me)

將以下內容加入 AI 助理的角色介紹（提示詞），以設定圖像生成工作流程：

<details>
<summary><strong>點擊展開提示詞範本</strong></summary>

```markdown
## 圖像生成工作流程

當用戶請求生成或是繪制圖像時，請依照以下步驟執行：

1. **撰寫提示詞**：將對話內容轉換為圖像生成提示詞
   - 要求畫作復刻，只有畫家名字與畫作名稱，原畫復刻，圖像只要畫作

2. **確認提示詞**：必需向用戶展示撰寫的提示詞與建議圖像方向
   - 畫作復刻要分析原圖方向

3. **執行生成**：必需等用戶確認後，呼叫 `aiIMG` MCP 工具並傳入參數

4. **回應**：當工具確定已在執行，回覆「圖像生成中」
```

</details>