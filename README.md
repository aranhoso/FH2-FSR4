# FH2-FSR4

**AMD FSR 4 Native AA (FSR Upscaling 4.1.1) para o Forza Horizon 2 rodando no [Azimuth](https://almost-there-games.itch.io/azimuth).**

O mod troca o MSAA do jogo pelo anti-aliasing temporal do FSR 4 em resolução nativa (sem upscaling). O resultado são bordas sem serrilhado, mais estabilidade em cercas, fios e vegetação, e uma imagem mais limpa em movimento.

É um mod não oficial, sem ligação com a AMD, a Microsoft, a Playground Games ou a Almost There Games.

## Requisitos

| | |
|---|---|
| Azimuth | 0.9.9 |
| Forza Horizon 2 | build 1.0.0.64 (a sua própria cópia, configurada no Azimuth) |
| GPU | AMD Radeon RX 9000 (RDNA 4). A RX 7000 (RDNA 3) também deve funcionar, porque a AMD liberou o FSR 4.1 para ela no SDK 2.3.0 |
| Driver | AMD Software: Adrenalin recente |

Testado até agora só numa **RX 9070 XT**, com Windows 11 e 2560x1440 em HDR. Em outras GPUs o loader da AMD escolhe outro provider, e isso ainda não foi testado.

## Instalação

1. Baixe o `.zip` da página de [Releases](../../releases).
2. Copie os três DLLs para a pasta do **Azimuth**, ao lado do `azimuth.exe`:
   - `dxgi.dll` (o mod)
   - `amd_fidelityfx_loader_dx12.dll` (AMD)
   - `amd_fidelityfx_upscaler_dx12.dll` (AMD, FSR 4.1.1)
3. No launcher do Azimuth, deixe o **anti-aliasing desligado**. O FSR passa a ser o anti-aliasing. Também dá para editar `data\azimuth.ini` e colocar `antialiasing=off`.
4. Jogue normalmente.

Para desinstalar, apague os três DLLs.

Funciona junto com o [FH2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions), que fica em `data\bin` e não conflita com este mod.

## Teclas

| Tecla | Ação |
|---|---|
| **F11** | Liga/desliga o FSR (bom para comparar) |
| F9 | Overlay de debug dos motion vectors: desligado, transparente ou opaco |
| F10 | Escala de brilho do overlay |

O log fica em `fh2_fsr.log`, na pasta do Azimuth.

## Como funciona

O FH2 nunca teve TAA, e o FSR precisa de quatro coisas que o jogo não entrega prontas. O mod é um proxy de `dxgi.dll` que faz hook nas vtables do D3D12 do Azimuth e monta cada uma delas:

- **Jitter de subpixel:** desloca o viewport das passadas da cena, só enquanto o depth buffer da cena está bound, seguindo a sequência oficial do FSR.
- **Motion vectors em resolução cheia:** o motion blur do FH2 já calcula a reprojeção da câmera e de cada carro (a matriz é escolhida pelo stencil de cada objeto), mas em meia resolução e com o vetor limitado. O mod roda uma variante desse shader em resolução cheia, sem esse limite, com os mesmos bindings do jogo.
- **Cor HDR e profundidade:** o mod pega os alvos do resolve da cena, antes do tonemap e do HUD.
- **Despacho do FSR:** o FSR roda dentro da própria command list do jogo, logo antes do tonemap. A saída é copiada de volta para a cor da cena, e depois o mod restaura heaps, root signatures, tabelas e PSO do jogo.

As passadas do jogo são identificadas pelo checksum DXBC dos pixel shaders, e o resolve também pelos formatos dos alvos.

## Limitações conhecidas

- Os shaders reconhecidos valem para o **FH2 1.0.0.64**. Storm Island e Fast & Furious podem precisar de ajustes.
- Rodas girando e vegetação balançando não têm motion vectors próprios.
- Não há máscara reativa, então partículas e transparências podem deixar um leve rastro.
- Os parâmetros de câmera passados ao FSR (near, far e FOV) são aproximados.

## Compilando

Precisa do [Zig 0.15.2](https://ziglang.org/download/), de Git e de um shell POSIX (o Git Bash no Windows serve).

```sh
ZIG=/caminho/para/zig ./build.sh           # build/dxgi.dll
ZIG=/caminho/para/zig ./build.sh release   # e dist/FH2-FSR4-<versão>.zip
```

O script baixa do repositório oficial da AMD os headers do FSR SDK v2.3.0. No modo `release`, baixa também os DLLs assinados da AMD.

## Licenças

- O código deste repositório está sob a licença MIT (veja [LICENSE](LICENSE)).
- Os DLLs do FSR são da AMD e são redistribuídos sem modificação, nos termos da licença do FSR SDK, que vai junto no zip de release como `LICENSE-AMD-FSR.txt`.
