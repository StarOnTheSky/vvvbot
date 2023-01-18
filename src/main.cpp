/*
    Source code of @vps_watermark_bot on Telegram

    参数:
    -t: (必需) Telegram Bot Token
    -d: (必需) Telegram 目标频道 ID/用户名
    -u: (必需) 允许使用的用户 ID, 多个用户用逗号分隔
    -w: 水印图片路径 (默认为 watermark.png)
    -a: 水印透明度 (默认为 0.5)
    -s: 保存图片到的路径 (默认不保存)

    保存的文件:
    - save_path/<datetime>/<file_id>.jpg: 加了水印的图片
    - save_path/<datetime>/orig_<file_id>.jpg: 原图
    - save_path/<datetime>/info.txt: 信息

    info.txt 格式:
    - Time: ISO 8601 格式的时间
    - User: uid
    - Message: 图片的描述
    - Images: 图片数量

    命令:
    /start: 开始使用
    /send: 开始发送图片
    /cancel: 取消发送
    /help: 显示帮助
*/

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>
#include <map>
#include <chrono>

#include <tgbot/tgbot.h>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace TgBot;

static cv::Mat Watermark;
static bool running = true;

struct _config
{
    string token;
    string channel;
    string watermark;
    double alpha;
    vector<int64> uids;
    string save_path;
};
static _config config;

struct _status
{
    int64 uid;
    bool sending;
    vector<string> images;
    int index;
    string datetime;
    string path;
    vector<string> media_id;
    string description;
};

static map<int64, _status> status; // User ID -> Status

bool auth(int64 id)
{
    return any_of(config.uids.begin(), config.uids.end(), [&](int64 uid)
                  { return uid == id; });
}

void signal_handler(const int signal)
{
    printf("收到信号 %d\n", signal);
    running = false;
    exit(0);
}

int add_watermark(const string &img)
{
    // Read the image
    cv::Mat image = cv::imread(img);
    if (image.empty())
    {
        printf("读取图片 %s 失败\n", img.c_str());
        return 1;
    }
    // Resize the watermark
    cv::Mat watermark_1;
    cv::Mat watermark;
    // Crop the watermark to the same aspect ratio as the image
    double ratio = (double)image.cols / image.rows;
    int w = Watermark.cols;
    int h = Watermark.rows;
    if (ratio < w / h)
    {
        w = (int)(h * ratio);
    }
    else
    {
        h = (int)(w / ratio);
    }
    // Ensure w and h < watermark original size
    if (w > Watermark.cols)
    {
        w = Watermark.cols;
    }
    if (h > Watermark.rows)
    {
        h = Watermark.rows;
    }
    watermark_1 = Watermark(cv::Rect(0, 0, w, h));
    cv::resize(watermark_1, watermark, cv::Size(image.cols, image.rows), 0, 0, cv::INTER_CUBIC);
    // Add the watermark
    cv::addWeighted(image, 1.0, watermark, config.alpha, 0.0, image);
    // Save the image
    cv::imwrite(img, image);
    return 0;
}

string iso8601()
{
    auto now = chrono::system_clock::now();
    auto in_time_t = chrono::system_clock::to_time_t(now);
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", localtime(&in_time_t));
    return string(buffer);
}

void handle_message(Bot &bot, const Message::Ptr message)
{
    // Check status
    if (!status.count(message->from->id))
    {
        // Ignore the messages
        return;
    }
    _status &s = status[message->from->id];
    // Check if the message is a photo
    if (message->photo.empty())
    {
        // Check if the message is a text
        if (message->text.empty())
        {
            // Unknown message, ignore it
            return;
        }
        s.description = message->text;
        if (s.images.empty())
        {
            bot.getApi().sendMessage(message->chat->id, "请先发送图片, 或者发送 /cancel 取消发送");
            return;
        }
        // Send the final message
        bot.getApi().sendMessage(message->chat->id, "正在发送图片, 请稍候...");

        // Send the images to the user, get the file IDs
        // and store them in order to send them to the channel without uploading them again
        
        for (const string &image : s.images)
        {
            auto photo_msg = bot.getApi().sendPhoto(message->chat->id, InputFile::fromFile(s.path + image, "image/jpeg"));
            s.media_id.push_back(photo_msg->photo.back()->fileId);
        }
        // Send the final message
        bot.getApi().sendMessage(message->chat->id, s.description);
        // Ask user to confirm
        auto keyboard(std::make_shared<InlineKeyboardMarkup>());
        keyboard->inlineKeyboard.emplace_back();
        keyboard->inlineKeyboard.back().push_back(std::make_shared<InlineKeyboardButton>());
        keyboard->inlineKeyboard.back().back()->text = "Yes";
        keyboard->inlineKeyboard.back().back()->callbackData = "yes";
        keyboard->inlineKeyboard.emplace_back();
        keyboard->inlineKeyboard.back().push_back(std::make_shared<InlineKeyboardButton>());
        keyboard->inlineKeyboard.back().back()->text = "No";
        keyboard->inlineKeyboard.back().back()->callbackData = "no";
        bot.getApi().sendMessage(message->chat->id, "确定要把上述内容转发到频道吗?", false, 0,
                                 keyboard, "Markdown");
        // Stop the conversation
        s.sending = false;
        // Start the confirmation
        bot.getEvents().onCallbackQuery([&s, &bot](CallbackQuery::Ptr query)
                                        {
                    if (query->data == "yes") {
                        // Send the images to the channel in media group with the description
                        vector<InputMedia::Ptr> media;
                        for (const string& id : s.media_id) {
                            auto input_media = std::make_shared<InputMediaPhoto>();
                            input_media->media = id;
                            input_media->caption = s.description;
                            media.push_back(input_media);
                        }
                        bot.getApi().sendMediaGroup(config.channel, media);
                        // Send the confirmation message
                        bot.getApi().sendMessage(s.uid, "图片已经发送到频道");
                    } else {
                        // Send the confirmation message
                        bot.getApi().sendMessage(s.uid, "图片发送已经取消");
                    }
                    // Delete the confirmation message
                    bot.getApi().deleteMessage(query->message->chat->id, query->message->messageId);

                    // Clean up
                    if (config.save_path.empty()) {
                        for (const string& image : s.images) {
                            remove(image.c_str());
                        }
                    } else {
                        // Write info.txt
                        string info = s.path + "info.txt";
                        auto f = fopen(info.c_str(), "w");
                        if (f == nullptr) {
                            printf("写入 %s 失败\n", info.c_str());
                        }
                        fprintf(f, "Time: %s\nUser: %ld\nMessage: %s\nImages: %ld", s.datetime.c_str(), s.uid, s.description.c_str(), s.images.size());
                        fclose(f);
                    }
                    status.erase(s.uid);
                    
                    // Stop the confirmation
                    bot.getEvents().onCallbackQuery(nullptr); });
        return;
    }
    // Check if the user is sending images
    if (!s.sending)
    {
        // Ignore the images
        return;
    }
    // Get the photo
    const PhotoSize::Ptr photo = message->photo.back();
    // Download the photo
    string filename = photo->fileId + ".jpg";
    try
    {
        auto file = bot.getApi().getFile(photo->fileId);
        auto f = fopen((s.path + filename).c_str(), "w");
        if (f == nullptr)
        {
            bot.getApi().sendMessage(message->chat->id, "打开文件失败");
            return;
        }
        auto content = bot.getApi().downloadFile(file->filePath);
        fwrite(content.data(), 1, content.size(), f);
        fclose(f);

        // Save an original copy
        if (!config.save_path.empty())
        {
            string original = s.path + "orig_" + filename;
            f = fopen(original.c_str(), "w");
            if (f == nullptr)
            {
                bot.getApi().sendMessage(message->chat->id, "打开文件失败");
                return;
            }
            fwrite(content.data(), 1, content.size(), f);
            fclose(f);
        }
    }
    catch (exception &e)
    {
        bot.getApi().sendMessage(message->chat->id, "下载图片失败");
        return;
    }

    // Add the watermark
    if (add_watermark(s.path + filename))
    {
        bot.getApi().sendMessage(message->chat->id, "添加水印失败");
        return;
    }

    // Save the image
    s.images.push_back(filename);
    bot.getApi().sendMessage(message->chat->id, "图片已添加");
}

int main(const int argc, const char **argv)
{
    // Parse arguments
    string uids_s;
    config.alpha = 0.5;
    for (int i = 1; i < argc; i++)
    {
        if (string(argv[i]) == "-t")
        {
            config.token = argv[++i];
        }
        else if (string(argv[i]) == "-d")
        {
            config.channel = argv[++i];
        }
        else if (string(argv[i]) == "-w")
        {
            config.watermark = argv[++i];
        }
        else if (string(argv[i]) == "-a")
        {
            config.alpha = atof(argv[++i]);
        }
        else if (string(argv[i]) == "-u")
        {
            uids_s = argv[++i];
        }
        else if (string(argv[i]) == "-s")
        {
            config.save_path = argv[++i];
        }
        else if (string(argv[i]) == "-h")
        {
            printf("使用方法: %s -t <token> -d <频道> -u <uids> [-w <水印图片>] [-a <透明度>] [-s <保存路径>]\n", argv[0]);
            return 0;
        }
        else
        {
            printf("未知参数 '%s'\n", argv[i]);
            return 1;
        }
    }
    if (config.token.empty() || config.channel.empty() || uids_s.empty())
    {
        printf("使用方法: %s -t <token> -d <频道> -u <uids> [-w <水印图片>] [-a <透明度>] [-s <保存路径>]\n", argv[0]);
        return 1;
    }

    // Check watermark file
    if (config.watermark.empty())
    {
        config.watermark = "watermark.png";
    }
    if ((Watermark = cv::imread(config.watermark)).empty())
    {
        printf("水印文件 '%s' 未找到\n", config.watermark.c_str());
        return 1;
    }

    // Check save path
    bool save = !config.save_path.empty();
    if (save)
    {
        config.save_path.append("/");
        // Test if the directory exists and writable, create if not exists
        if (access(config.save_path.c_str(), F_OK) != 0)
        {
            if (mkdir(config.save_path.c_str(), 0777) != 0)
            {
                printf("无法创建目录 '%s'\n", config.save_path.c_str());
                return 1;
            }
        }
        else if (access(config.save_path.c_str(), W_OK) != 0)
        {
            printf("目录 '%s' 不可写\n", config.save_path.c_str());
            return 1;
        }
    }

    // Parse UIDs
    size_t pos = 0;
    while (true)
    {
        size_t next = uids_s.find(',', pos);
        if (next == string::npos)
        {
            config.uids.push_back(atoi(uids_s.substr(pos).c_str()));
            break;
        }
        config.uids.push_back(atoll(uids_s.substr(pos, next - pos).c_str()));
        pos = next + 1;
    }

    // Print the arguments
    printf("Token: %s****\n", config.token.substr(0, 10).c_str()); // Hide part of token to keep secure
    printf("目标频道: %s\n", config.channel.c_str());
    printf("水印文件: %s\n", config.watermark.c_str());
    printf("透明度: %f\n", config.alpha);
    printf("允许的 UID: ");
    for (auto uid : config.uids)
    {
        printf("%ld, ", uid);
    }
    printf("\n");
    if (save)
    {
        printf("保存路径: %s\n", config.save_path.c_str());
    }
    else
    {
        printf("保存路径: 不保存\n");
    }

    // Create the bot
    Bot bot(config.token);

    // Register the signal handler
    signal(SIGINT, signal_handler);

    // Register the /start command
    bot.getEvents().onCommand("start", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            bot.getApi().sendMessage(message->chat->id, "You are not allowed to use this bot");
            return;
        }
        // Send the welcome message
        string msg = "欢迎使用 @" +  bot.getApi().getMe()->username + "!";
        bot.getApi().sendMessage(message->chat->id, msg);
        bot.getApi().sendMessage(message->chat->id, "发送 '/send' 来开始"); });

    // Register the /send command
    bot.getEvents().onCommand("send", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        bot.getApi().sendMessage(message->chat->id, 
            "请将需要添加水印的图片发送给我，"
            "图片发送完成以后, 请发送一段描述文字作为结束,"
            "或者发送 '/cancel' 来取消");
        // Start the conversation
        status[message->from->id].uid = message->from->id;
        status[message->from->id].sending = true;
        status[message->from->id].datetime = iso8601();
        string path = "";
        if (!config.save_path.empty())
        {
            path = config.save_path + status[message->from->id].datetime + "/";
            if (mkdir(path.c_str(), 0777) != 0)
            {
                bot.getApi().sendMessage(message->chat->id, "无法创建目录");
                return;
            }
        }
        status[message->from->id].path = path; });

    // Register the /cancel command
    bot.getEvents().onCommand("cancel", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        // Cancel the conversation
        status.erase(message->from->id);
        bot.getApi().sendMessage(message->chat->id, "已取消"); });

    // Register the /help command
    bot.getEvents().onCommand("help", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        // Send the help message
        string msg = "使用 /send 来开始发送图片\n"
                     "使用 /cancel 来取消发送\n"
                     "使用 /help 来显示帮助信息\n"
                     "使用 /start 来显示欢迎信息";
        bot.getApi().sendMessage(message->chat->id, msg); });

    // Register the message handler
    bot.getEvents().onAnyMessage([&bot](Message::Ptr message)
                                 { handle_message(bot, message); });

    // Start the bot
    printf("机器人用户名: %s\n", bot.getApi().getMe()->username.c_str());
    TgLongPoll longPoll(bot);
    while (running)
    {
        printf("Long poll started\n");
        longPoll.start();
    }
    return 0;
}
