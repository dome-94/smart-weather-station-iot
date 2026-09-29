%% =============================================================
%   MATLAB Analysis — Dự đoán thời tiết (Linear Regression)
%   Dán vào: ThingSpeak > Apps > MATLAB Analysis > New > Blank
% =============================================================
%   Dùng 1 CHANNEL DUY NHẤT: đọc field1(Temp),field3(Pressure)
%   để hồi quy, rồi ghi kết quả vào field6,7,8 của CHÍNH channel đó.
%
%   ► ĐIỀN 3 GIÁ TRỊ DƯỚI THEO CHANNEL "WeatherStation" CỦA BẠN:
% =============================================================

channelID   = 0000000;               % Channel ID (số trên URL ThingSpeak)
readAPIKey  = 'YOUR_READ_API_KEY';
writeAPIKey = 'YOUR_WRITE_API_KEY';

numPoints = 180;   % Số điểm gần nhất dùng để hồi quy (180 điểm * 60s = 3 giờ)

% ── 1. Đọc lịch sử Temp (field1) + Pressure (field3) ─────────────
try
    data = thingSpeakRead(channelID, 'Fields', [1, 3], ...
        'NumPoints', numPoints, 'ReadKey', readAPIKey, ...
        'OutputFormat', 'table');
catch ME
    error('Không đọc được dữ liệu: %s', ME.message);
end

if height(data) < 3
    warning('Chưa đủ dữ liệu để hồi quy (cần ≥ 3 điểm) — bỏ qua lần này.');
    return;
end

% ── 2. Trích xuất dữ liệu an toàn (tránh lỗi lệch tên cột trên ThingSpeak) ──
if ismember('Temperature', data.Properties.VariableNames)
    temp = data.Temperature;
elseif ismember('field1', data.Properties.VariableNames)
    temp = data.field1;
elseif ismember('Field1', data.Properties.VariableNames)
    temp = data.Field1;
else
    temp = data{:, 1}; % Fallback lấy cột thứ nhất
end

if ismember('Pressure', data.Properties.VariableNames)
    pres = data.Pressure;
elseif ismember('field3', data.Properties.VariableNames)
    pres = data.field3;
elseif ismember('Field3', data.Properties.VariableNames)
    pres = data.Field3;
else
    pres = data{:, 2}; % Fallback lấy cột thứ hai
end

t = (1:length(temp))';

% ── Lọc nhiễu cảm biến (Data Sanitization) ──
% Lọc nhiệt độ ngoài khoảng vật lý [0, 55°C]
temp(temp < 0 | temp > 55) = NaN;
% Lọc khí áp ngoài khoảng vật lý [950, 1050 hPa]
pres(pres < 950 | pres > 1050) = NaN;

% Điền khuyết các giá trị NaN bằng phương pháp điền trước/sau (fillmissing)
temp = fillmissing(temp, 'previous');
temp = fillmissing(temp, 'next');
pres = fillmissing(pres, 'previous');
pres = fillmissing(pres, 'next');

% ── 2. Hồi quy tuyến tính: dự đoán nhiệt độ chu kỳ tới ───────────
pTemp    = polyfit(t, temp, 1);
nextTemp = polyval(pTemp, length(temp) + 1);
nextTemp = max(min(nextTemp, max(temp) + 5), min(temp) - 5); % Giới hạn ngoại suy

% ── 3. Xu hướng áp suất → ước lượng khả năng mưa ─────────────────
pPres         = polyfit(t, pres, 1);
pressureSlope = pPres(1);

if pressureSlope < -0.1
    rainProb = 80;
elseif pressureSlope < -0.03
    rainProb = 50;
elseif pressureSlope > 0.1
    rainProb = 5;
else
    rainProb = 20;
end

% ── 4. Ghi kết quả vào field6,7,8 CỦA CÙNG CHANNEL ───────────────
try
    % Ghi [nextTemp, rainProb] vào các trường tương ứng (ngừng ghi field8 xu hướng khí áp để tránh xung đột độ ẩm dự báo AI)
    thingSpeakWrite(channelID, [nextTemp, rainProb], ...
        'Fields', [6, 7], 'WriteKey', writeAPIKey);
catch ME
    error('Không ghi được kết quả: %s', ME.message);
end

fprintf('Dự đoán nhiệt độ chu kỳ tới : %.2f °C\n', nextTemp);
fprintf('Khả năng mưa                : %d %%\n',   rainProb);
fprintf('Xu hướng áp suất             : %.3f hPa/chu kỳ\n', pressureSlope);
