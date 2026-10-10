# HD 化調査メモ: System40.ini の ViewWidth/Height 方式 (凍結時点の記録)

## 結論: 最有力アプローチ

`System40.ini` の `ViewWidth`/`ViewHeight` を 1600/1200 に書き換えるだけで、
**エンジンがネイティブに 1600×1200 座標系でシーンを合成する**ことが確認された。
vtable フックや bb 差し替えによる座標変換は一切不要。

- スプライト位置は view 比でスケールされる（スクリプトは 800×600 前提の
  ままでよい）
- スプライトサイズはアセットの実ピクセル数で描かれる → **全アセットを
  2x 化すれば位置もサイズも揃う**
- アニメーション・遷移・パーティクルは全て正常動作
- present (BitBlt→ウィンドウ) は既存のミラー+StretchBlt で対応可能
  （proxy 側は `GAME_W/H` を ini 値から読むよう変更済み）

検証実績（System40.ini = 1600×1200、proxy の HD 変換は全て無効化）:

| 構成 | 結果 |
|---|---|
| オリジナル素材 | 全画面正常。全要素が 1x サイズ・半分位置で表示 |
| 我々の全 2x 変換 GM (work/Rance7GM_our2x.ald) | タイトルはロゴ・背景・箱・花びらがほぼ正常。文字のみ 1x |
| Rance7GM_hd2x.ald (外部製) | 同上 + 冒頭画面が白 |

## 未解決の問題

1. **テキストが 1x のまま**
   - フォントサイズは AIN スクリプト指定のピクセル値で固定
     (xsystem4 `src/hll/SACT2.c` の `sact_text_metrics_to_text_style` 参照。
     `SP_SetTextSize` 系 HLL の引数を ×2 するフックが想定解)
   - メニュー文字が箱とずれて左上に表示されるのが典型症状

2. **冒頭画面 (注意文・MangaGamer/KAGURA ロゴ) が白い**
   - 2x CG 使用時のみ発生（オリジナル CG では表示される）。
     我々の正規エンコードでも hd2x でも同じ → アーカイブ破損ではない
   - 白画面期間はウィンドウへの BitBlt が完全にゼロ → シーン構築自体が
     失敗している可能性 (固定 800×600 内部バッファ経路? 要調査)
   - 回避策候補: 冒頭で使う CG だけ 1x のまま残す（半分サイズ表示になるが
     表示はされる）

3. **ALICESOFT プラグインアニメの表示位置**
   - AliceLogo*.dll 系プラグインが独自に描画 → 左上に小さく出る。
     プラグイン DLL の IAT はロード後パッチが必要（現状未対応）

## 廃止したアプローチ

vtable フックによる座標変換・中間 surface 拡大は断念。
副作用（アニメーション停止・クラッシュ）が大きく、本質的解決にならない。
`dinput_proxy.c` の関連コードは `feat/hd-renderscale-stepwise` ブランチに
残っている（RenderScale>1 でのみ有効化されるようゲート済み）。

## 作業中に混入した実験コードの状態

- `g_viewW/g_viewH`: System40.ini から ViewWidth/Height を読み
  present 基準サイズに使用。ini 未変更時は 800×600 で従来と同じ動作
- `blit:` ログ / StretchBlt 観察フック / surf/bb ダンプ: デバッグ用。
  凍結解除時に整理要
- `g_slots[]` のオーバーフロー対策: MAX_RD_SLOTS=384 + 上限ガード
  （vtable 全スロットフック時に発生していたクラッシュの原因だった）

## 再開時の最初のステップ

1. MultiRes.ini で Window サイズ指定のみ有効化して ini=1600 運用を整える
2. SACT2 のテキスト HLL (SP_SetTextSize 等) をフックしてフォントサイズ×2
3. 冒頭画面のシーン構築失敗原因を DecodeCG/SurfaceFactory の生成ログで追跡
4. 全画面巡回テスト（ADV 本文・戦闘・マップ等の固定 800×600 経路洗い出し）

## 追記 (2026-10-10): 冒頭注意文/会社ロゴの 1600×1200 表示 — 成功

### 特定した CG 番号 (Rance7.ain ダンプより)

- `ShowWarningScreen` → CG **64990** (注意文、800×600 黒地テキスト)
- `ShowCompanyLogo` → CG **64991** (MangaGamer), **64992** (KAGURA)
- 呼び出し元: `tagLogo@run` → `RunLogos` (FUNC 4497-4499)

### 実機検証結果

`Game_Sample` にて System40.ini=1600×1200 + cg64990-64993 のみ
2x (Lanczos) に差し替えた GM を投入:

- **注意文: 1600×1200 全面表示を確認** (白画面なし)。`SP_SET_CG` で
  位置 0,0 に実寸描画されるため、2x 画像がちょうど view 全面を覆う
- **MangaGamer ロゴも同様に全面表示を確認**
- タイトル画面は「1x 素材が左上 800×600 相当 + パーティクル (位置は
  view 比スケール) が全画面」という想定通りの混在状態

### 「白画面」問題の再評価

本テスト (冒頭系 4 枚のみ 2x) では白画面は再現しなかった。
従来観察された白画面は「冒頭 CG そのもの」ではなく、
**全量 2x GM に含まれる別のアセット** (恐らく AliceLogo3 プラグイン
アニメが参照する素材、または別の固定 800×600 経路で使われるもの)
が原因の可能性が高い。範囲を絞った差し替えで切り分け可能。

### 高 DPI 環境での依存解消 (重要)

プロキシ (dinput.dll) なしでも `System40.ini` + 差し替え ald のみで
1600×1200 動作することを確認したが、**DPI スケーリング >100% の
環境では OS がウィンドウを 1.5 倍などに拡大し、エンジンの
BitBlt 領域とクライアントが不一致**になって表示が崩れる
(左上に実寸描画 + 残り白)。

対策として以下を確認済み (4K/150% 環境):

- `HKCU\...\AppCompatFlags\Layers` に
  `<Rance7.exeへのフルパス> = "HIGHDPIAWARE"` を登録すると、
  プロセスが DPI aware になりクライアントが実ピクセル 1600×1200
  で作られ、**DLL なしで完全に正しく表示される**
- 外部マニフェスト (`Rance7.exe.manifest`) は exe 側の埋め込み
  マニフェストが優先されるため効かなかった
- `patch_staging/install.bat` / `uninstall.bat` に登録/解除
  スクリプトを置いた (exe フルパスが必要なため .reg では不可)

### スクショ取得の注意 (4K/150% DPI 環境)

- `CopyFromScreen` では PrintWindow ともに **呼び出し元プロセスの
  DPI awareness が結果を左右する**。DPI unaware なプロセスから
  取得した GetClientRect は論理 px (実寸/1.5) を返し、クライアント
  左上 2/3 しか取れない
- 対策: PowerShell 側で先に `SetProcessDpiAwarenessContext(-4)`
  (PER_MONITOR_AWARE_V2) を呼んでから GetClientRect + PrintWindow

### 残タスク (冒頭まわり)

- ~~AliceLogo3.dll プラグインアニメ (ALICESOFT ロゴ) の表示位置/サイズ~~
  → 下記「AliceLogo3.alk の 2x 化」で解決
- cg64993 (4枚目、白 800×600 1 枚絵?) の用途確認 — 一応 2x 化済み

### AliceLogo3.alk の 2x 化 (2026-10-10: 成功)

`ShowAliceLogo` は `CALLHLL AliceLogo3 Init/Run` でプラグイン
(`DLL/AliceLogo3.dll`) に委譲する。描画リソースはゲームルートの
`AliceLogo3.alk` (9.8MB)。解析したところ:

- 形式は **ALK0 アーカイブ**: `'ALK0'` + u32 count + (u32 offset,
  u32 size) ×count テーブル + QNT データ連結 (無アライン)
- 全 107 エントリが QNT。内訳: 800×600 のフレーム 31 枚 +
  パーツ (はなまるポーズ・紙吹雪・ロゴ文字等) 76 枚。全件 x,y=0

全エントリを 2x (Lanczos) エンコードし ALK0 を再パックして
ゲームに置いた結果、**プラグインアニメが 1600×1200 全面で
正しく再生されることを実機確認**。プラグインは各 QNT の実寸を
使って配置計算しているらしく、画像 2x 化だけで合成がそのまま
view に揃う。

補助ツール: `rance7_hd_asset_patch/tools/alk.py`
(list/extract/pack を実装)

軽微な観察: シーンによって画面中央 (x≈800) にごく薄い縦線が
残る場合がある。トランジション拭き取りの境界線と思われる
(未修飾版との比較未実施)。
