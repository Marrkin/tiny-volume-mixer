# TinyVolumeMixer

Um mixer de volume pequeno e nativo para Windows 11, com acesso pela bandeja perto do relógio.

## Como usar

1. Extraia a versão portátil e execute `TinyVolumeMixer.exe`.
2. Clique no ícone da bandeja ou use **Ctrl + Alt + M**.
3. Ajuste o volume geral ou o volume de um aplicativo. Clique no ícone ao lado da barra para silenciar.
4. Clique fora da janela, no X ou pressione **Esc** para ocultar. O processo continua na bandeja.

O botão com três linhas muda a orientação das barras. O botão circular de tema permite escolher **Automático (Windows)**, **Claro** ou **Escuro**. O modo automático acompanha mudanças no tema de aplicativos do Windows enquanto o mixer está em execução. O botão de configurações abre as opções de atalho e início automático. As preferências ficam salvas para o seu usuário. O início automático vem desligado.

Se o atalho escolhido já estiver em uso, uma mensagem aparece e o atalho anterior continua funcionando. No menu do botão direito da bandeja, **Sair** encerra o aplicativo.

As barras seguem o mixer clássico do Windows. Ao baixar o geral, os aplicativos daquela saída descem proporcionalmente. Por exemplo, com o geral em **50%**, um app que utiliza todo o volume disponível aparece em **50%**; um app que utiliza metade aparece em **25%**. Ao aumentar um app acima do geral, o geral sobe junto e os demais apps mantêm o nível que estavam exibindo. Levar o geral a zero e depois aumentá-lo preserva as proporções entre os aplicativos.

Programas com várias sessões de áudio compartilham uma barra. Quando os volumes são diferentes, mostramos o maior; mover a barra aplica o volume a todas as sessões daquele programa. Aumentar a barra acima de zero também retira o mudo. Sessões pausadas continuam visíveis enquanto existirem no Windows.

O **volume geral** acompanha a saída multimídia padrão. Os **aplicativos** incluem sessões em todas as saídas ativas, mesmo quando um programa está usando outro dispositivo. Cada aplicativo acompanha o volume da saída onde está tocando; um aplicativo em outro dispositivo pode aparecer acima do geral da saída padrão. Ao trocar entre fone e caixas de som, a lista acompanha as notificações do Windows. Sem uma saída disponível, a janela informa a situação e oferece tentar novamente.

Aplicativos da Microsoft Store usam o nome e o ícone do cadastro do Windows. O áudio de um componente WebView2 aparece no controle do aplicativo que o utiliza, quando essa identificação está disponível. Assim, o WhatsApp pode ter suas sessões reunidas em **WhatsApp**, em vez de exibir **WhatsApp.Root** e uma linha separada do runtime.

O aplicativo usa um ícone próprio. Ele não modifica o ícone original de volume do Windows.

## Compilar

É necessário Visual Studio 2022 com a carga **Desenvolvimento para desktop com C++**, Windows SDK e CMake 3.24 ou superior:

```powershell
cmake --preset windows-x64
cmake --build --preset release
ctest --preset release
.\build\windows-x64\Release\TinyVolumeMixer.exe
```

Para iniciar sem abrir a janela, use `TinyVolumeMixer.exe --background`.

O GitHub Actions prepara o ZIP portátil e o checksum. Uma tag de versão, como `v1.0.0`, aciona a publicação da release; um build local não publica arquivos no GitHub.

## Desinstalar

Desative **Iniciar com o Windows**, clique em **Sair** e apague o executável. As preferências podem ser removidas em `HKCU\Software\TinyVolumeMixer`. Nenhuma permissão de administrador é necessária para o uso normal.
