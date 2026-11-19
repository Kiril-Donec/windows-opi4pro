# WPF window for Install-A733Windows.ps1 (English / Russian). Dot-sourced by the
# main script and uses its functions (Get-CandidateDisks, Format-Disk, Mount-Iso,
# Dismount-Iso, Find-InstallImage, Get-Editions). The actual work runs in a
# hidden child PowerShell ("-NoGui -Yes ..."), whose transcript is followed here.

$script:GuiText = @{
    en = @{
        Title = 'Windows 11 for Orange Pi 4 Pro'
        Sub = 'SD card installer  ·  Allwinner A733  ·  v{0}'
        Badge = 'EXPERIMENTAL'
        WarnBold = 'Everything on the selected card will be erased.'
        WarnRest = ' Windows runs slowly on an SD card: this is an experimental project.'
        S1Title = 'SD card'; S1Hint = '32 GB or more, in a card reader of this PC'; Refresh = 'Refresh'
        S2Title = 'Windows 11 ARM64 image'; S2HintA = 'ISO from '; S2HintB = ', or a folder / USB drive with the extracted ISO'
        Iso = 'ISO...'; Folder = 'Folder...'; NotSelected = 'not selected'
        S3Title = 'Edition'; S3Hint = 'Pro is recommended'
        Agree = 'I understand that the card will be erased'
        Start = 'Write card'
        NoCard = 'No SD card found (15 GB - 1 TB, card reader or USB). Insert the card and press Refresh.'
        ImgErr = 'Windows image: {0}'
        NoArm = 'no ARM64 Windows image here. Download "Windows 11 (Arm64)" from microsoft.com.'
        FolderDesc = 'Folder or USB drive with the extracted Windows 11 ARM64 ISO (contains sources\install.wim)'
        Confirm = "Erase this card and write Windows 11 ({0})?`n`n{1}"; ConfirmTitle = 'Confirm'
        ProgTitle = 'Writing the card...'; ProgSub = 'Do not remove the card. This takes 5-15 minutes.'
        Steps = @('Read the Windows image', 'Prepare the installer (WinPE + A733 drivers)', 'Write boot firmware and partition table',
                  'Format the card', 'Copy the installer boot files', 'Copy the Windows image', 'Final check')
        Starting = 'Starting...'; Copying = 'Copying the Windows image: {0:N0}%'
        DoneTitle = 'Done'; DoneSub = 'You can remove the card now.'; DoneHead = 'Card ready'
        DoneText = 'Put the card into the Orange Pi 4 Pro, connect HDMI, keyboard and mouse (white USB port under the blue one), optionally Ethernet, and power it on. The board installs Windows by itself in 20-60 minutes, then reboots into the Windows setup screens. Do not power it off meanwhile.'
        FailTitle = 'Failed'; FailHead = 'Something went wrong'; FailExit = 'the installer stopped (exit code {0}); see the details'
        Details = 'Details'; OpenLog = 'Open log file'; Close = 'Close'
        StopQ = "The card is being written. Stopping now leaves it unusable (you can run the installer again).`n`nStop anyway?"; StopTitle = 'Stop?'
    }
    ru = @{
        Title = 'Windows 11 для Orange Pi 4 Pro'
        Sub = 'Установщик на SD-карту  ·  Allwinner A733  ·  v{0}'
        Badge = 'ЭКСПЕРИМЕНТ'
        WarnBold = 'Всё на выбранной карте будет стёрто.'
        WarnRest = ' С SD-карты Windows работает медленно: это экспериментальный проект.'
        S1Title = 'SD-карта'; S1Hint = 'От 32 ГБ, в кардридере этого компьютера'; Refresh = 'Обновить'
        S2Title = 'Образ Windows 11 ARM64'; S2HintA = 'ISO с сайта '; S2HintB = ' или папка / флешка с распакованным ISO'
        Iso = 'ISO...'; Folder = 'Папка...'; NotSelected = 'не выбран'
        S3Title = 'Редакция'; S3Hint = 'Рекомендуется Pro'
        Agree = 'Я понимаю, что карта будет стёрта'
        Start = 'Записать карту'
        NoCard = 'SD-карта не найдена (15 ГБ - 1 ТБ, кардридер или USB). Вставьте карту и нажмите «Обновить».'
        ImgErr = 'Образ Windows: {0}'
        NoArm = 'здесь нет образа Windows для ARM64. Скачайте «Windows 11 (Arm64)» с microsoft.com.'
        FolderDesc = 'Папка или флешка с распакованным ISO Windows 11 ARM64 (в ней есть sources\install.wim)'
        Confirm = "Стереть эту карту и записать Windows 11 ({0})?`n`n{1}"; ConfirmTitle = 'Подтверждение'
        ProgTitle = 'Запись карты...'; ProgSub = 'Не вынимайте карту. Это займёт 5-15 минут.'
        Steps = @('Чтение образа Windows', 'Подготовка установщика (WinPE + драйверы A733)', 'Запись загрузчика и таблицы разделов',
                  'Форматирование карты', 'Копирование загрузочных файлов', 'Копирование образа Windows', 'Финальная проверка')
        Starting = 'Запуск...'; Copying = 'Копирование образа Windows: {0:N0}%'
        DoneTitle = 'Готово'; DoneSub = 'Карту можно вынимать.'; DoneHead = 'Карта готова'
        DoneText = 'Вставьте карту в Orange Pi 4 Pro, подключите HDMI, клавиатуру и мышь (белый USB-порт под синим), по желанию Ethernet, и включите плату. Она сама установит Windows за 20-60 минут и перезагрузится в обычную настройку Windows. Не выключайте её в это время.'
        FailTitle = 'Не получилось'; FailHead = 'Что-то пошло не так'; FailExit = 'установщик остановился (код {0}), смотрите подробности'
        Details = 'Подробности'; OpenLog = 'Открыть лог'; Close = 'Закрыть'
        StopQ = "Карта сейчас записывается. Если остановить, она будет непригодна (установщик можно запустить снова).`n`nОстановить?"; StopTitle = 'Остановить?'
    }
}

$script:GuiStepKeys = @('Reading the ISO', 'Preparing the installer WinPE', 'Writing the boot firmware', 'Formatting',
                        'Copying the installer boot files', 'Copying the Windows image', 'Final check')

$script:GuiXaml = @'
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Width="760" SizeToContent="Height" WindowStartupLocation="CenterScreen" ResizeMode="CanMinimize"
        Background="#F4F6F9" FontFamily="Segoe UI" FontSize="13" Foreground="#1F2937">
  <Window.Icon>
    <DrawingImage>
      <DrawingImage.Drawing>
        <DrawingGroup>
          <GeometryDrawing Brush="#EA580C"><GeometryDrawing.Geometry><RectangleGeometry Rect="0,0,32,32" RadiusX="7" RadiusY="7"/></GeometryDrawing.Geometry></GeometryDrawing>
          <GeometryDrawing Brush="White"><GeometryDrawing.Geometry><RectangleGeometry Rect="7,7,8.5,8.5"/></GeometryDrawing.Geometry></GeometryDrawing>
          <GeometryDrawing Brush="White"><GeometryDrawing.Geometry><RectangleGeometry Rect="16.5,7,8.5,8.5"/></GeometryDrawing.Geometry></GeometryDrawing>
          <GeometryDrawing Brush="White"><GeometryDrawing.Geometry><RectangleGeometry Rect="7,16.5,8.5,8.5"/></GeometryDrawing.Geometry></GeometryDrawing>
          <GeometryDrawing Brush="White"><GeometryDrawing.Geometry><RectangleGeometry Rect="16.5,16.5,8.5,8.5"/></GeometryDrawing.Geometry></GeometryDrawing>
        </DrawingGroup>
      </DrawingImage.Drawing>
    </DrawingImage>
  </Window.Icon>
  <Window.Resources>
    <Style x:Key="Card" TargetType="Border">
      <Setter Property="Background" Value="White"/>
      <Setter Property="CornerRadius" Value="10"/>
      <Setter Property="Padding" Value="16,14"/>
      <Setter Property="Margin" Value="0,0,0,10"/>
      <Setter Property="BorderBrush" Value="#E2E6EC"/>
      <Setter Property="BorderThickness" Value="1"/>
    </Style>
    <Style x:Key="Num" TargetType="Border">
      <Setter Property="Width" Value="26"/>
      <Setter Property="Height" Value="26"/>
      <Setter Property="CornerRadius" Value="13"/>
      <Setter Property="Background" Value="#FDE7D9"/>
      <Setter Property="Margin" Value="0,0,12,0"/>
      <Setter Property="VerticalAlignment" Value="Top"/>
    </Style>
    <Style x:Key="Btn" TargetType="Button">
      <Setter Property="Padding" Value="14,6"/>
      <Setter Property="Background" Value="White"/>
      <Setter Property="BorderBrush" Value="#CBD2DC"/>
      <Setter Property="Foreground" Value="#1F2937"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="MinWidth" Value="84"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}"
                    BorderThickness="1" CornerRadius="6" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#EEF1F5"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.45"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Accent" TargetType="Button">
      <Setter Property="Padding" Value="26,10"/>
      <Setter Property="Foreground" Value="White"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="FontSize" Value="14"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border x:Name="b" Background="#EA580C" CornerRadius="7" Padding="{TemplateBinding Padding}">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="b" Property="Background" Value="#C2410C"/></Trigger>
              <Trigger Property="IsEnabled" Value="False"><Setter TargetName="b" Property="Background" Value="#F3B28F"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="ComboBox">
      <Setter Property="Foreground" Value="#1F2937"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ComboBox">
            <Grid>
              <ToggleButton Focusable="False" ClickMode="Press"
                            IsChecked="{Binding IsDropDownOpen, Mode=TwoWay, RelativeSource={RelativeSource TemplatedParent}}">
                <ToggleButton.Template>
                  <ControlTemplate TargetType="ToggleButton">
                    <Border x:Name="Bd" Background="White" BorderBrush="#CBD2DC" BorderThickness="1" CornerRadius="6">
                      <Path HorizontalAlignment="Right" VerticalAlignment="Center" Margin="0,0,12,0"
                            Data="M 0 0 L 5 5 L 10 0" Stroke="#6B7280" StrokeThickness="1.6"/>
                    </Border>
                    <ControlTemplate.Triggers>
                      <Trigger Property="IsMouseOver" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="#F59E0B"/></Trigger>
                      <Trigger Property="IsChecked" Value="True"><Setter TargetName="Bd" Property="BorderBrush" Value="#EA580C"/></Trigger>
                    </ControlTemplate.Triggers>
                  </ControlTemplate>
                </ToggleButton.Template>
              </ToggleButton>
              <ContentPresenter IsHitTestVisible="False" Margin="10,0,32,0" VerticalAlignment="Center" HorizontalAlignment="Left"
                                Content="{TemplateBinding SelectionBoxItem}" ContentTemplate="{TemplateBinding SelectionBoxItemTemplate}"/>
              <Popup IsOpen="{TemplateBinding IsDropDownOpen}" Placement="Bottom" AllowsTransparency="True" Focusable="False" PopupAnimation="Fade">
                <Border Background="White" BorderBrush="#E2E6EC" BorderThickness="1" CornerRadius="8" Margin="0,4,10,10" Padding="4"
                        MinWidth="{Binding ActualWidth, RelativeSource={RelativeSource TemplatedParent}}" MaxHeight="280">
                  <Border.Effect><DropShadowEffect BlurRadius="14" ShadowDepth="3" Opacity="0.16"/></Border.Effect>
                  <ScrollViewer VerticalScrollBarVisibility="Auto"><ItemsPresenter/></ScrollViewer>
                </Border>
              </Popup>
            </Grid>
            <ControlTemplate.Triggers>
              <Trigger Property="IsEnabled" Value="False"><Setter Property="Opacity" Value="0.5"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style TargetType="ComboBoxItem">
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="ComboBoxItem">
            <Border x:Name="Bd" CornerRadius="5" Padding="10,7" Background="Transparent">
              <ContentPresenter/>
            </Border>
            <ControlTemplate.Triggers>
              <Trigger Property="IsHighlighted" Value="True"><Setter TargetName="Bd" Property="Background" Value="#FFF1E6"/></Trigger>
              <Trigger Property="IsSelected" Value="True"><Setter TargetName="Bd" Property="Background" Value="#FDE7D9"/></Trigger>
            </ControlTemplate.Triggers>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
    <Style x:Key="Lang" TargetType="Button">
      <Setter Property="Foreground" Value="White"/>
      <Setter Property="FontSize" Value="11.5"/>
      <Setter Property="FontWeight" Value="SemiBold"/>
      <Setter Property="Cursor" Value="Hand"/>
      <Setter Property="Background" Value="Transparent"/>
      <Setter Property="Template">
        <Setter.Value>
          <ControlTemplate TargetType="Button">
            <Border Background="{TemplateBinding Background}" CornerRadius="9" Padding="10,2">
              <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
            </Border>
          </ControlTemplate>
        </Setter.Value>
      </Setter>
    </Style>
  </Window.Resources>

  <StackPanel>
    <Border Padding="26,18,22,18">
      <Border.Background>
        <LinearGradientBrush StartPoint="0,0" EndPoint="1,1">
          <GradientStop Color="#EA580C" Offset="0"/>
          <GradientStop Color="#F59E0B" Offset="1"/>
        </LinearGradientBrush>
      </Border.Background>
      <Grid>
        <StackPanel Margin="0,2,0,0">
          <TextBlock x:Name="HeadTitle" FontSize="23" FontWeight="SemiBold" Foreground="White"/>
          <TextBlock x:Name="SubTitle" Foreground="#FFF1E6" Margin="0,3,0,0"/>
        </StackPanel>
        <StackPanel HorizontalAlignment="Right" VerticalAlignment="Top" Orientation="Horizontal">
          <Border Background="#33FFFFFF" CornerRadius="10" Padding="10,3" Margin="0,0,10,0" VerticalAlignment="Center">
            <TextBlock x:Name="Badge" Foreground="White" FontSize="11" FontWeight="SemiBold"/>
          </Border>
          <Border Background="#26000000" CornerRadius="11" Padding="2" VerticalAlignment="Center">
            <StackPanel Orientation="Horizontal">
              <Button x:Name="LangEn" Style="{StaticResource Lang}" Content="EN"/>
              <Button x:Name="LangRu" Style="{StaticResource Lang}" Content="RU"/>
            </StackPanel>
          </Border>
        </StackPanel>
      </Grid>
    </Border>

    <!-- ===================== setup page ===================== -->
    <StackPanel x:Name="SetupPage" Margin="24,16,24,22">
      <Border Background="#FFF7ED" BorderBrush="#FDBA74" BorderThickness="1" CornerRadius="8" Padding="12,9" Margin="0,0,0,12">
        <TextBlock TextWrapping="Wrap" Foreground="#9A3412">
          <Run x:Name="WarnBold" FontWeight="SemiBold"/><Run x:Name="WarnRest"/>
        </TextBlock>
      </Border>

      <Border Style="{StaticResource Card}">
        <DockPanel>
          <Border Style="{StaticResource Num}"><TextBlock Text="1" Foreground="#C2410C" FontWeight="Bold" HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
          <StackPanel>
            <TextBlock x:Name="S1Title" FontWeight="SemiBold" FontSize="14"/>
            <TextBlock x:Name="S1Hint" Foreground="#6B7280" Margin="0,1,0,8"/>
            <DockPanel>
              <Button x:Name="RefreshBtn" DockPanel.Dock="Right" Style="{StaticResource Btn}" Margin="8,0,0,0"/>
              <ComboBox x:Name="DiskBox" Height="32" VerticalContentAlignment="Center"/>
            </DockPanel>
          </StackPanel>
        </DockPanel>
      </Border>

      <Border Style="{StaticResource Card}">
        <DockPanel>
          <Border Style="{StaticResource Num}"><TextBlock Text="2" Foreground="#C2410C" FontWeight="Bold" HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
          <StackPanel>
            <TextBlock x:Name="S2Title" FontWeight="SemiBold" FontSize="14"/>
            <TextBlock Foreground="#6B7280" Margin="0,1,0,8" TextWrapping="Wrap">
              <Run x:Name="S2HintA"/><Hyperlink x:Name="MsLink" NavigateUri="https://www.microsoft.com/software-download/windows11arm64">microsoft.com</Hyperlink><Run x:Name="S2HintB"/>
            </TextBlock>
            <DockPanel>
              <Button x:Name="FolderBtn" DockPanel.Dock="Right" Style="{StaticResource Btn}" Margin="8,0,0,0"/>
              <Button x:Name="IsoBtn" DockPanel.Dock="Right" Style="{StaticResource Btn}" Margin="8,0,0,0"/>
              <Border BorderBrush="#CBD2DC" BorderThickness="1" CornerRadius="4" Background="#F9FAFB" Height="30">
                <TextBlock x:Name="SourceText" Foreground="#9CA3AF" VerticalAlignment="Center" Margin="8,0" TextTrimming="CharacterEllipsis"/>
              </Border>
            </DockPanel>
          </StackPanel>
        </DockPanel>
      </Border>

      <Border Style="{StaticResource Card}">
        <DockPanel>
          <Border Style="{StaticResource Num}"><TextBlock Text="3" Foreground="#C2410C" FontWeight="Bold" HorizontalAlignment="Center" VerticalAlignment="Center"/></Border>
          <StackPanel>
            <TextBlock x:Name="S3Title" FontWeight="SemiBold" FontSize="14"/>
            <TextBlock x:Name="S3Hint" Foreground="#6B7280" Margin="0,1,0,8"/>
            <ComboBox x:Name="EditionBox" Height="32" VerticalContentAlignment="Center"/>
          </StackPanel>
        </DockPanel>
      </Border>

      <TextBlock x:Name="Status" Foreground="#B91C1C" TextWrapping="Wrap" Margin="2,2,0,10" Visibility="Collapsed"/>

      <DockPanel Margin="0,4,0,0">
        <Button x:Name="StartBtn" DockPanel.Dock="Right" Style="{StaticResource Accent}" IsEnabled="False"/>
        <CheckBox x:Name="AgreeBox" VerticalAlignment="Center" VerticalContentAlignment="Center">
          <TextBlock x:Name="AgreeText" TextWrapping="Wrap"/>
        </CheckBox>
      </DockPanel>
    </StackPanel>

    <!-- ===================== progress page ===================== -->
    <StackPanel x:Name="ProgressPage" Margin="24,18,24,22" Visibility="Collapsed">
      <TextBlock x:Name="ProgTitle" FontSize="18" FontWeight="SemiBold"/>
      <TextBlock x:Name="ProgSub" Foreground="#6B7280" Margin="0,2,0,14" TextWrapping="Wrap"/>
      <Border Style="{StaticResource Card}">
        <StackPanel x:Name="StepPanel"/>
      </Border>
      <ProgressBar x:Name="Bar" Height="8" Minimum="0" Maximum="100" Foreground="#EA580C" Background="#E5E7EB" BorderThickness="0" Margin="0,4,0,6"/>
      <TextBlock x:Name="ProgText" Foreground="#6B7280" Text=" "/>

      <Border x:Name="DonePanel" Background="#ECFDF5" BorderBrush="#6EE7B7" BorderThickness="1" CornerRadius="8" Padding="14,10" Margin="0,12,0,0" Visibility="Collapsed">
        <StackPanel>
          <TextBlock x:Name="DoneHead" FontWeight="SemiBold" Foreground="#065F46" FontSize="14"/>
          <TextBlock x:Name="DoneText" TextWrapping="Wrap" Foreground="#065F46" Margin="0,4,0,0"/>
        </StackPanel>
      </Border>
      <Border x:Name="FailPanel" Background="#FEF2F2" BorderBrush="#FCA5A5" BorderThickness="1" CornerRadius="8" Padding="14,10" Margin="0,12,0,0" Visibility="Collapsed">
        <StackPanel>
          <TextBlock x:Name="FailHead" FontWeight="SemiBold" Foreground="#991B1B" FontSize="14"/>
          <TextBlock x:Name="FailText" TextWrapping="Wrap" Foreground="#991B1B" Margin="0,4,0,0"/>
        </StackPanel>
      </Border>

      <Expander x:Name="LogExpander" Margin="0,12,0,0">
        <TextBox x:Name="LogBox" Height="150" IsReadOnly="True" FontFamily="Consolas" FontSize="11.5"
                 VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Auto" Margin="0,6,0,0"
                 Background="#111827" Foreground="#E5E7EB" BorderThickness="0" Padding="6"/>
      </Expander>
      <DockPanel Margin="0,14,0,0">
        <Button x:Name="CloseBtn" DockPanel.Dock="Right" Style="{StaticResource Btn}" IsEnabled="False"/>
        <Button x:Name="LogBtn" DockPanel.Dock="Left" Style="{StaticResource Btn}" HorizontalAlignment="Left"/>
      </DockPanel>
    </StackPanel>
  </StackPanel>
</Window>
'@

function Show-A733Gui {
    param([string]$ScriptPath, [string]$Version, [string]$LogPath, [string]$Screenshot, [switch]$AllowVirtual, [string]$Language)
    Add-Type -AssemblyName PresentationFramework, PresentationCore, WindowsBase, System.Windows.Forms, System.Drawing
    $reader = New-Object System.Xml.XmlNodeReader ([xml]$script:GuiXaml)
    $w = [Windows.Markup.XamlReader]::Load($reader)
    $ui = @{}
    foreach ($n in 'HeadTitle', 'SubTitle', 'Badge', 'LangEn', 'LangRu', 'SetupPage', 'ProgressPage', 'WarnBold', 'WarnRest',
                   'S1Title', 'S1Hint', 'RefreshBtn', 'DiskBox', 'S2Title', 'S2HintA', 'S2HintB', 'MsLink', 'FolderBtn', 'IsoBtn',
                   'SourceText', 'S3Title', 'S3Hint', 'EditionBox', 'Status', 'StartBtn', 'AgreeBox', 'AgreeText',
                   'ProgTitle', 'ProgSub', 'StepPanel', 'Bar', 'ProgText', 'DonePanel', 'DoneHead', 'DoneText',
                   'FailPanel', 'FailHead', 'FailText', 'LogExpander', 'LogBox', 'CloseBtn', 'LogBtn') {
        $ui[$n] = $w.FindName($n)
    }
    if (-not $Language) {
        $Language = if (@('ru', 'uk', 'be') -contains [Globalization.CultureInfo]::CurrentUICulture.TwoLetterISOLanguageName) { 'ru' } else { 'en' }
    }
    $st = @{
        Lang = $Language; Disks = @(); Editions = @(); Source = $null; Proc = $null; Running = $false; Result = 1
        Phase = 'setup'; Current = -1; Pct = $null; Err = $null; StatusKey = $null; StatusArg = $null; Icons = @(); Labels = @()
    }
    $T = { param($k) $script:GuiText[$st.Lang][$k] }

    $renderSteps = {
        $steps = & $T 'Steps'
        for ($i = 0; $i -lt $st.Labels.Count; $i++) {
            $st.Labels[$i].Text = $steps[$i]
            $done = ($i -lt $st.Current) -or ($i -eq $st.Current -and $st.Phase -eq 'done') -or ($st.Phase -eq 'done')
            if ($done) {
                $st.Icons[$i].Text = [string][char]0x2714; $st.Icons[$i].Foreground = '#059669'; $st.Labels[$i].Foreground = '#1F2937'; $st.Labels[$i].FontWeight = 'Normal'
            } elseif ($i -eq $st.Current -and $st.Phase -eq 'fail') {
                $st.Icons[$i].Text = [string][char]0x2716; $st.Icons[$i].Foreground = '#DC2626'; $st.Labels[$i].Foreground = '#991B1B'; $st.Labels[$i].FontWeight = 'SemiBold'
            } elseif ($i -eq $st.Current) {
                $st.Icons[$i].Text = [string][char]0x25C9; $st.Icons[$i].Foreground = '#EA580C'; $st.Labels[$i].Foreground = '#1F2937'; $st.Labels[$i].FontWeight = 'SemiBold'
            } else {
                $st.Icons[$i].Text = [string][char]0x25CB; $st.Icons[$i].Foreground = '#9CA3AF'; $st.Labels[$i].Foreground = '#9CA3AF'; $st.Labels[$i].FontWeight = 'Normal'
            }
        }
    }
    $renderProgress = {
        switch ($st.Phase) {
            'done' {
                $ui.ProgTitle.Text = & $T 'DoneTitle'; $ui.ProgSub.Text = & $T 'DoneSub'; $ui.ProgText.Text = ' '
            }
            'fail' {
                $ui.ProgTitle.Text = & $T 'FailTitle'; $ui.ProgSub.Text = $LogPath; $ui.ProgText.Text = ' '
                $ui.FailText.Text = if ($st.Err) { $st.Err } else { (& $T 'FailExit') -f $st.Proc.ExitCode }
            }
            default {
                $ui.ProgTitle.Text = & $T 'ProgTitle'; $ui.ProgSub.Text = & $T 'ProgSub'
                if ($null -ne $st.Pct -and $st.Current -eq 5) { $ui.ProgText.Text = (& $T 'Copying') -f $st.Pct }
                elseif ($st.Current -ge 0) { $ui.ProgText.Text = (& $T 'Steps')[$st.Current] + '...' }
                else { $ui.ProgText.Text = & $T 'Starting' }
            }
        }
        & $renderSteps
    }
    $applyLang = {
        $w.Title = & $T 'Title'
        $ui.HeadTitle.Text = & $T 'Title'
        $ui.SubTitle.Text = (& $T 'Sub') -f $Version
        $ui.Badge.Text = & $T 'Badge'
        foreach ($p in @(@($ui.LangEn, 'en'), @($ui.LangRu, 'ru'))) {
            $p[0].Background = if ($st.Lang -eq $p[1]) { '#FFFFFF' } else { 'Transparent' }
            $p[0].Foreground = if ($st.Lang -eq $p[1]) { '#C2410C' } else { '#FFFFFF' }
        }
        $ui.WarnBold.Text = & $T 'WarnBold'; $ui.WarnRest.Text = & $T 'WarnRest'
        $ui.S1Title.Text = & $T 'S1Title'; $ui.S1Hint.Text = & $T 'S1Hint'; $ui.RefreshBtn.Content = & $T 'Refresh'
        $ui.S2Title.Text = & $T 'S2Title'; $ui.S2HintA.Text = & $T 'S2HintA'; $ui.S2HintB.Text = & $T 'S2HintB'
        $ui.IsoBtn.Content = & $T 'Iso'; $ui.FolderBtn.Content = & $T 'Folder'
        if (-not $st.Source -and $ui.SourceText.Tag -ne 'set') { $ui.SourceText.Text = & $T 'NotSelected' }
        $ui.S3Title.Text = & $T 'S3Title'; $ui.S3Hint.Text = & $T 'S3Hint'
        $ui.AgreeText.Text = & $T 'Agree'; $ui.StartBtn.Content = & $T 'Start'
        if ($st.StatusKey) { $ui.Status.Text = (& $T $st.StatusKey) -f $st.StatusArg }
        $ui.DoneHead.Text = & $T 'DoneHead'; $ui.DoneText.Text = & $T 'DoneText'; $ui.FailHead.Text = & $T 'FailHead'
        $ui.LogExpander.Header = & $T 'Details'; $ui.LogBtn.Content = & $T 'OpenLog'; $ui.CloseBtn.Content = & $T 'Close'
        if ($st.Phase -ne 'setup') { & $renderProgress }
    }
    $setStatus = {
        param([string]$key, $arg)
        $st.StatusKey = $key; $st.StatusArg = $arg
        if ($key) { $ui.Status.Text = (& $T $key) -f $arg; $ui.Status.Visibility = 'Visible' } else { $ui.Status.Visibility = 'Collapsed' }
    }
    $update = { $ui.StartBtn.IsEnabled = ($ui.DiskBox.SelectedIndex -ge 0 -and $ui.EditionBox.SelectedIndex -ge 0 -and $ui.AgreeBox.IsChecked) }
    $fill = {
        $ui.DiskBox.Items.Clear()
        $st.Disks = @(Get-CandidateDisks)
        foreach ($d in $st.Disks) { [void]$ui.DiskBox.Items.Add((Format-Disk $d)) }
        if ($st.Disks.Count -eq 1) { $ui.DiskBox.SelectedIndex = 0 }
        if ($st.Disks.Count) { if ($st.StatusKey -eq 'NoCard') { & $setStatus $null } } else { & $setStatus 'NoCard' }
        & $update
    }
    $load = {
        param([string]$path)
        $ui.SourceText.Text = $path; $ui.SourceText.Tag = 'set'; $ui.SourceText.Foreground = '#1F2937'
        $ui.EditionBox.Items.Clear(); $st.Editions = @(); $st.Source = $null
        $w.Cursor = [Windows.Input.Cursors]::Wait
        try {
            $root = Mount-Iso $path
            try { $st.Editions = @(Get-Editions (Find-InstallImage $root) | Where-Object { $_.Arch -eq 12 }) } finally { Dismount-Iso $path }
            if (-not $st.Editions.Count) { & $setStatus 'ImgErr' (& $T 'NoArm') }
            else {
                $pick = 0
                for ($i = 0; $i -lt $st.Editions.Count; $i++) {
                    [void]$ui.EditionBox.Items.Add($st.Editions[$i].Name)
                    if ($st.Editions[$i].Name -match 'Pro$') { $pick = $i }
                }
                $ui.EditionBox.SelectedIndex = $pick
                $st.Source = $path
                if ($st.StatusKey -eq 'ImgErr') { & $setStatus $null }
            }
        } catch { & $setStatus 'ImgErr' $_.Exception.Message }
        finally { $w.Cursor = $null }
        & $update
    }

    $ui.LangEn.Add_Click({ $st.Lang = 'en'; & $applyLang })
    $ui.LangRu.Add_Click({ $st.Lang = 'ru'; & $applyLang })
    $ui.RefreshBtn.Add_Click($fill)
    $ui.DiskBox.Add_SelectionChanged($update)
    $ui.EditionBox.Add_SelectionChanged($update)
    $ui.AgreeBox.Add_Click($update)
    $ui.MsLink.Add_RequestNavigate({ param($s, $e) Start-Process $e.Uri.AbsoluteUri })
    $ui.IsoBtn.Add_Click({
        $dlg = New-Object Microsoft.Win32.OpenFileDialog
        $dlg.Filter = 'ISO (*.iso)|*.iso'
        if ($dlg.ShowDialog($w)) { & $load $dlg.FileName }
    })
    $ui.FolderBtn.Add_Click({
        $dlg = New-Object Windows.Forms.FolderBrowserDialog
        $dlg.Description = & $T 'FolderDesc'
        if ($dlg.ShowDialog() -eq 'OK') { & $load $dlg.SelectedPath }
    })
    $ui.LogBtn.Add_Click({ if (Test-Path $LogPath) { Start-Process notepad.exe $LogPath } })
    $ui.CloseBtn.Add_Click({ $w.Close() })

    # ---------------- progress (follows the worker's transcript)
    $buildSteps = {
        $ui.StepPanel.Children.Clear(); $st.Icons = @(); $st.Labels = @()
        foreach ($k in $script:GuiStepKeys) {
            $row = New-Object Windows.Controls.DockPanel; $row.Margin = '0,3,0,3'
            $ic = New-Object Windows.Controls.TextBlock; $ic.Width = 26; $ic.FontSize = 14
            $lb = New-Object Windows.Controls.TextBlock
            [void]$row.Children.Add($ic); [void]$row.Children.Add($lb); [void]$ui.StepPanel.Children.Add($row)
            $st.Icons += $ic; $st.Labels += $lb
        }
    }
    $readLog = {
        if (-not (Test-Path $LogPath)) { return , @() }
        try {
            $fs = New-Object IO.FileStream($LogPath, 'Open', 'Read', 'ReadWrite')
            $sr = New-Object IO.StreamReader($fs, $true)
            $text = $sr.ReadToEnd(); $sr.Dispose()
            , ($text -split "`r?`n")
        } catch { , @() }
    }
    $skip = '^\*{10}|^(Windows PowerShell transcript|Start time|End time|Username|RunAs User|Configuration Name|Machine|Host Application|Process ID|PSVersion|PSEdition|PSCompatibleVersions|BuildVersion|CLRVersion|WSManStackVersion|PSRemotingProtocolVersion|SerializationVersion)'
    $tick = {
        $current = -1; $pct = $null; $err = $null; $done = $false; $log = New-Object Text.StringBuilder
        foreach ($l in (& $readLog)) {
            if ($l -match '^=== \d\d:\d\d:\d\d (.+)$') {
                $t = $Matches[1]; $pct = $null
                for ($i = 0; $i -lt $script:GuiStepKeys.Count; $i++) { if ($t.StartsWith($script:GuiStepKeys[$i]) -and $i -gt $current) { $current = $i } }
            }
            if ($l -match '^\s*(\d+(\.\d+)?)%\s*$') { $pct = [double]$Matches[1]; continue }
            if ($l -match '^ERROR: (.+)$') { $err = $Matches[1] }
            if ($l -match '^DONE\.') { $done = $true }
            if ($l -notmatch $skip) { [void]$log.AppendLine($l) }
        }
        $txt = $log.ToString().Trim()
        if ($ui.LogBox.Text -ne $txt) { $ui.LogBox.Text = $txt; $ui.LogBox.ScrollToEnd() }
        $st.Current = $current; $st.Pct = $pct
        $exited = ($null -ne $st.Proc -and $st.Proc.HasExited)
        if ($done -or ($exited -and $st.Proc.ExitCode -eq 0)) {
            $st.Phase = 'done'; $st.Current = $script:GuiStepKeys.Count - 1
            $ui.Bar.IsIndeterminate = $false; $ui.Bar.Value = 100; $ui.DonePanel.Visibility = 'Visible'
        } elseif ($err -or $exited) {
            $st.Phase = 'fail'; $st.Err = $err; $st.Current = [Math]::Max($current, 0)
            $ui.Bar.IsIndeterminate = $false; $ui.Bar.Value = 0; $ui.FailPanel.Visibility = 'Visible'; $ui.LogExpander.IsExpanded = $true
        } else {
            if ($null -ne $pct -and $current -eq 5) { $ui.Bar.IsIndeterminate = $false; $ui.Bar.Value = $pct } else { $ui.Bar.IsIndeterminate = $true }
        }
        & $renderProgress
        if ($st.Phase -ne 'run') { $st.Running = $false; $st.Result = [int]($st.Phase -ne 'done'); $ui.CloseBtn.IsEnabled = $true; $st.Timer.Stop() }
    }

    $ui.StartBtn.Add_Click({
        $d = $st.Disks[$ui.DiskBox.SelectedIndex]
        $e = $st.Editions[$ui.EditionBox.SelectedIndex]
        $msg = (& $T 'Confirm') -f $e.Name, (Format-Disk $d)
        if ([Windows.MessageBox]::Show($w, $msg, (& $T 'ConfirmTitle'), 'YesNo', 'Warning') -ne 'Yes') { return }
        $args2 = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$ScriptPath`"", '-NoGui', '-Yes',
                   '-DiskNumber', $d.Number, '-IsoPath', "`"$($st.Source)`"", '-ImageIndex', $e.Index, '-LogPath', "`"$LogPath`"")
        if ($AllowVirtual) { $args2 += '-AllowVirtualDisk' }
        if ($script:GuiExtraArgs) { $args2 += $script:GuiExtraArgs }
        $st.Proc = Start-Process powershell.exe -ArgumentList $args2 -WindowStyle Hidden -PassThru
        $null = $st.Proc.Handle    # keep ExitCode readable after exit
        $st.Running = $true; $st.Phase = 'run'; $st.Current = -1
        $ui.SetupPage.Visibility = 'Collapsed'; $ui.ProgressPage.Visibility = 'Visible'
        & $buildSteps
        & $renderProgress
        $ui.Bar.IsIndeterminate = $true
        $st.Timer.Start()
    })
    $st.Timer = New-Object Windows.Threading.DispatcherTimer
    $st.Timer.Interval = [TimeSpan]::FromMilliseconds(500)
    $st.Timer.Add_Tick($tick)

    $w.Add_Closing({
        param($s, $e)
        if ($st.Running) {
            if ([Windows.MessageBox]::Show($w, (& $T 'StopQ'), (& $T 'StopTitle'), 'YesNo', 'Warning') -ne 'Yes') { $e.Cancel = $true; return }
            try { $st.Proc.Kill() } catch { }
        }
    })

    & $applyLang
    $w.Add_ContentRendered({
        & $fill
        if ($Screenshot) {
            if ($script:GuiScreenshotVariant -eq 'progress') {
                $st.Phase = 'run'; $st.Current = 5; $st.Pct = 62
                $ui.SetupPage.Visibility = 'Collapsed'; $ui.ProgressPage.Visibility = 'Visible'
                & $buildSteps; & $renderProgress
                $ui.Bar.Value = 62
            } else {
                & $setStatus $null
                $ui.DiskBox.Items.Clear(); [void]$ui.DiskBox.Items.Add('2: Generic STORAGE DEVICE - 61,9 GB (USB)'); $ui.DiskBox.SelectedIndex = 0
                $ui.SourceText.Text = 'D:\Downloads\Win11_25H2_English_Arm64.iso'; $ui.SourceText.Tag = 'set'; $ui.SourceText.Foreground = '#1F2937'
                $ui.EditionBox.Items.Clear(); [void]$ui.EditionBox.Items.Add('Windows 11 Pro'); $ui.EditionBox.SelectedIndex = 0
                $ui.AgreeBox.IsChecked = $true; $ui.StartBtn.IsEnabled = $true
            }
            Save-A733GuiScreenshot $w $Screenshot
            $w.Close()
        }
    })
    [void]$w.ShowDialog()
    $st.Result
}

# Docs: capture the window (with its Windows 11 frame) on a plain backdrop.
function Save-A733GuiScreenshot($w, [string]$Path) {
    $w.Topmost = $true
    $w.UpdateLayout()
    $src = [Windows.PresentationSource]::FromVisual($w)
    $sx = $src.CompositionTarget.TransformToDevice.M11; $sy = $src.CompositionTarget.TransformToDevice.M22
    $pad = 36
    $back = New-Object Windows.Window
    $back.WindowStyle = 'None'; $back.ResizeMode = 'NoResize'; $back.ShowInTaskbar = $false
    $back.Background = '#E2E8F0'
    $back.Left = $w.Left - $pad; $back.Top = $w.Top - $pad
    $back.Width = $w.ActualWidth + 2 * $pad; $back.Height = $w.ActualHeight + 2 * $pad
    $back.Topmost = $true
    $back.Show()
    $w.Topmost = $false; $w.Topmost = $true    # re-raise above the backdrop
    $w.Activate()
    for ($i = 0; $i -lt 12; $i++) {
        [Windows.Forms.Application]::DoEvents()
        $w.Dispatcher.Invoke([Action] {}, [Windows.Threading.DispatcherPriority]::Background)
        Start-Sleep -Milliseconds 80
    }
    $x = [int](($w.Left - $pad) * $sx); $y = [int](($w.Top - $pad) * $sy)
    $cw = [int](($w.ActualWidth + 2 * $pad) * $sx); $ch = [int](($w.ActualHeight + 2 * $pad) * $sy)
    $bmp = New-Object Drawing.Bitmap($cw, $ch)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object Drawing.Size($cw, $ch)))
    $g.Dispose()
    $bmp.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $back.Close()
}
