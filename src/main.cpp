/*
    @vps_watermark_bot 的源代码

    参数:
    -t: (必需) Telegram Bot 的 Token
    -d: (Required) The channel to send the watermarked images to
    -o: (Required) The channel to send the original images to
    -u: (Required) Allowed UIDs (separated by commas ',')
    -w: The watermark image file to add to the images (default is watermark.png)
    -a: The alpha value of the watermark (default is 0.5)
    -s: Save the images to destination directory (default not to save them)

    Saved files:
    - save_path/<datetime>/<file_id>.jpg: The watermarked images
    - save_path/<datetime>/orig_<file_id>.jpg: The original images
    - save_path/<datetime>/info.txt: The information of the images

    Content of info.txt:
    - Time: <time>
    - User: <user>
    - Message: <message>
    - Images: <file ids of the photos>

    命令:
    /start: 开始
    /send: 发送图片
    /cancel: 取消发送
    /watermark <标签>: 回复某一条消息, 然后机器人会用对应标签的水印图组来回复这个消息
    /modify <标签> <新内容>: 修改某一条消息的内容
    /help: 显示帮助信息

*/

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>
#include <map>
#include <chrono>
#include <thread>
#include <fstream>

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
    string channel_orig;
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
    vector<string> media_orig_id;
    string description;
};
static map<int64, _status> status; // User ID -> Status

struct _messages_to_delete {
    int64 chat_id;
    int message_id;
    time_t timestamp;
};
static queue<_messages_to_delete> messages_to_delete; // Messages to delete after 5s (message ID, timestamp)
void pending_delete(Bot &bot) {
    while (running) {
        this_thread::sleep_for(chrono::seconds(1)); // Sleep for 1s
        while (!messages_to_delete.empty()) {
            auto message = messages_to_delete.front();
            try {
                if (message.timestamp + 5 >= chrono::system_clock::to_time_t(chrono::system_clock::now())) {
                    // The following messages are not ready to be deleted
                    break;
                }

                bot.getApi().deleteMessage(message.chat_id, message.message_id);
                messages_to_delete.pop();
            } catch (exception &e) {
                printf("Failed to delete message %d: %s\n", message.message_id, e.what());
            }
        }
    }
}
void add_temp_message(const Message::Ptr &message) {
    _messages_to_delete m;
    m.chat_id = message->chat->id;
    m.message_id = message->messageId;
    m.timestamp = message->date;
    messages_to_delete.push(m);
}

bool auth(int64 id)
{
    for (int64 uid : config.uids)
    {
        if (id == uid)
        {
            return true;
        }
    }
    return false;
}

void signal_handler(const int signal)
{
    printf("Received signal %d\n", signal);
    running = false;
    exit(0);
}

int add_watermark(const string &img)
{
    // Read the image
    cv::Mat image = cv::imread(img);
    if (image.empty())
    {
        printf("Failed to read image %s\n", img.c_str());
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

    if (message->chat != nullptr && message->chat->type != Chat::Type::Private)
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
            bot.getApi().sendMessage(message->chat->id, "请先发送图片");
            return;
        }
        // Send the final message
        bot.getApi().sendMessage(message->chat->id, "正在发送图片...");

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
        bot.getApi().sendMessage(message->chat->id, "确定要发送到频道吗？", false, 0,
                                 keyboard, "Markdown");
        // Stop the conversation
        s.sending = false;
        // Start the confirmation
        bot.getEvents().onCallbackQuery([&s, &bot](CallbackQuery::Ptr query)
                                        {
                    if (query->data == "yes") {
                        // Send the images to the channel in media group with the description
                        vector<InputMedia::Ptr> media;
                        bool first = true;
                        for (const string& id : s.media_id) {
                            auto input_media = std::make_shared<InputMediaPhoto>();
                            input_media->media = id;
                            if (first) {
                                input_media->caption = s.description + "\n标签： " + s.datetime;
                                first = false;
                            }
                            input_media->hasSpoiler = false;
                            media.push_back(input_media);
                        }
                        bot.getApi().sendMediaGroup(config.channel, media);
                        media.clear();
                        first = true;
                        for (const string& id : s.media_orig_id) {
                            auto input_media = std::make_shared<InputMediaPhoto>();
                            input_media->media = id;
                            if (first) {
                                input_media->caption = s.description;
                                first = false;
                            }
                            input_media->hasSpoiler = false;
                            media.push_back(input_media);
                        }
                        // Send the confirmation message
                        bot.getApi().sendMessage(s.uid, "图片已发送至频道");
                        bot.getApi().sendMessage(s.uid, "标签: `" + s.datetime + "`", false, 0, nullptr, "Markdown"); // Send the tag in markdown so that it can be copied
                    } else {
                        // Send the confirmation message
                        bot.getApi().sendMessage(s.uid, "取消");
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
                        string info = "Time: " + s.datetime + "\nUser: " + to_string(s.uid) + "\nDescription: " + s.description + "\nImages: ";
                        for (const string& image : s.images) {
                            info += image + " ";
                        }
                        info += "\n";
                        fstream f(config.save_path + s.datetime + "/info.txt", ios::out);
                        f << info;
                        f.close();
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
    s.media_orig_id.push_back(photo->fileId);
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
        bot.getApi().sendMessage(message->chat->id, "图片下载失败");
        return;
    }

    // Add the watermark
    if (add_watermark(s.path + filename))
    {
        bot.getApi().sendMessage(message->chat->id, "水印添加失败");
        return;
    }

    // Save the image
    s.images.push_back(filename);
    bot.getApi().sendMessage(message->chat->id, "图片已添加");
}

void handle_watermark(Bot &bot, Message::Ptr message)
{
    // This command must reply to a message
    if (message->replyToMessage == nullptr)
    {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "请回复一条消息"));
        return;
    }
    // Truncate the command
    istringstream command_iss(message->text);
    string datetime;
    try {
        getline(command_iss, datetime, ' ');
        getline(command_iss, datetime, ' ');
    } catch (exception &e) {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "命令无效"));
        return;
    }
    // Argument: a datetime string sent by the bot in "/send" command
    string path = config.save_path + datetime + "/";
    if (access(path.c_str(), F_OK) != 0)
    {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "参数无效"));
        return;
    }
    // Read the info.txt to get photo file ids and the description
    fstream f(path + "info.txt");
    if (!f.is_open())
    {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "打开文件失败"));
        return;
    }
    try {
        // Datetime
        string datetime_f;
        getline(f, datetime_f);

        // User
        string user_f;
        getline(f, user_f);

        // Description
        string description_f;
        getline(f, description_f);
        if (!description_f.starts_with("Description: ")) {
            throw exception();
        }
        string description = description_f.substr(13);

        // Images
        string images_f;
        getline(f, images_f, '\n');
        if (!images_f.starts_with("Images: ")) {
            throw exception();
        }
        vector<string> images;
        string image;
        istringstream iss(images_f.substr(8));
        while (getline(iss, image, ' ')) {
            // Remove the ".jpg" suffix
            image = image.substr(0, image.size() - 4);
            images.push_back(image);
        }

        f.close();

        // Upload
        vector<InputMedia::Ptr> media;
        bool first = true;
        for (const string& img : images) {
            auto m = make_shared<InputMediaPhoto>();
            m->media = img;
            if (first) {
                m->caption = description;
                first = false;
            }
            m->hasSpoiler = false;
            media.push_back(m);
        }
        bot.getApi().sendMediaGroup(message->chat->id, media, false, message->replyToMessage->messageId);
        add_temp_message(message);
        return;
    }
    catch (exception &e) {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "Wrong file format"));
        return;
    }
}

void modify_desc(Bot &bot, Message::Ptr message)
{    
    if (message->chat != nullptr && message->chat->type != Chat::Type::Private)
    {
        // Ignore the messages
        return;
    }
    // 解析命令
    istringstream command_iss(message->text);
    string datetime;
    string new_desc;
    try {
        getline(command_iss, datetime, ' '); // "/modify"
        getline(command_iss, datetime, ' '); // datetime
        command_iss >> new_desc; // 新的描述
        // 将描述中的换行符替换为空格
        for (char &c : new_desc)
            if (c == '\n')
                c = ' ';

        // 检查描述是否为空
        if (new_desc.empty())
            throw exception();
    } catch (exception &e) {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "命令无效"));
        return;
    }
    // 检查路径是否存在
    string path = config.save_path + datetime + "/";
    if (access(path.c_str(), F_OK) != 0)
    {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "参数无效"));
        return;
    }
    // 读取 info.txt 
    fstream f(path + "info.txt", ios::in | ios::out);
    if (!f.is_open())
    {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "打开文件失败"));
        return;
    }
    // 读取文件内容, 并修改描述
    vector<string> lines;
    string line;
    string orig_desc;
    try {
        while (getline(f, line))
        {
            if (line.starts_with("Description: "))
            {
                orig_desc = line.substr(13);
                line = "Description: " + new_desc;
            }
            lines.push_back(line);
        }
        f.close();
    }
    catch (exception &e) {
        bot.getApi().sendMessage(message->chat->id, "文件格式错误");
        f.close();
        return;
    }
    // 写入文件
    f.open(path + "info.txt", ios::out);
    if (!f.is_open())
    {
        add_temp_message(message);
        add_temp_message(bot.getApi().sendMessage(message->chat->id, "打开文件失败"));
        return;
    }
    try {
        for (const string& l : lines)
        {
            f << l << endl;
        }
        f.close();
    }
    catch (exception &e) {
        bot.getApi().sendMessage(message->chat->id, "文件写入失败");
        f.close();
        return;
    }
    
    // 修改成功
    bot.getApi().sendMessage(message->chat->id, "修改成功");
    bot.getApi().sendMessage(message->chat->id, "原描述: " + orig_desc);
    bot.getApi().sendMessage(message->chat->id, "新描述: " + new_desc);
    return;
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
        else if (string(argv[i]) == "-o") 
        {
            config.channel_orig = argv[++i];
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
            printf("Usage: %s -t <token> -d <channel> -u <uids> [-w <watermark>] [-a <alpha>] [-s <path/to/save>]\n", argv[0]);
            return 0;
        }
        else
        {
            printf("Unknown argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (config.token.empty() || config.channel.empty() || uids_s.empty())
    {
        printf("Usage: %s -t <token> -d <channel> -u <uids> [-w <watermark>] [-a <alpha>]\n", argv[0]);
        return 1;
    }

    // Check watermark file
    if (config.watermark.empty())
    {
        config.watermark = "watermark.png";
    }
    if ((Watermark = cv::imread(config.watermark)).empty())
    {
        printf("Watermark file '%s' not found\n", config.watermark.c_str());
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
                printf("Failed to create the directory '%s'\n", config.save_path.c_str());
                return 1;
            }
        }
        else if (access(config.save_path.c_str(), W_OK) != 0)
        {
            printf("The directory '%s' is not writable\n", config.save_path.c_str());
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
    printf("Destnation Channel: %s\n", config.channel.c_str());
    printf("Watermark file: %s\n", config.watermark.c_str());
    printf("Alpha: %f\n", config.alpha);
    printf("Allowed UIDs: ");
    for (auto uid : config.uids)
    {
        printf("%ld, ", uid);
    }
    printf("\n");
    if (save)
    {
        printf("Save path: %s\n", config.save_path.c_str());
    }
    else
    {
        printf("Save path: Not saving\n");
    }

    // Create the bot
    Bot bot(config.token);

    // Register the signal handler
    signal(SIGINT, signal_handler);

    // Register the /start command
    bot.getEvents().onCommand("start", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            bot.getApi().sendMessage(message->chat->id, "你无法使用这个 bot");
            return;
        }
        // Send the welcome message
        string msg = "欢迎使用 @" +  bot.getApi().getMe()->username + "!";
        bot.getApi().sendMessage(message->chat->id, msg);
        bot.getApi().sendMessage(message->chat->id, "输入 '/send' 开始发送"); });

    // Register the /send command
    bot.getEvents().onCommand("send", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        // If not private chat, ignore the message
        if (message->chat->type != Chat::Type::Private)
        {
            add_temp_message(message);
            add_temp_message(bot.getApi().sendMessage(message->chat->id, "请私聊我使用此命令"));
            return;
        }

        bot.getApi().sendMessage(message->chat->id, 
            "发送要添加水印的图片，"
            "最后发送一条描述文字，"
            "或者发送 '/cancel' 取消发送");
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
                bot.getApi().sendMessage(message->chat->id, "创建目录失败");
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
        bot.getApi().sendMessage(message->chat->id, "取消"); });

    // Register the /watermark command
    bot.getEvents().onCommand("watermark", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        handle_watermark(bot, message); });
    
    // Register the /modify command
    bot.getEvents().onCommand("modify", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        modify_desc(bot, message); });

    // Register the /help command
    bot.getEvents().onCommand("help", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        // Send the help message
        string msg = "私聊发送 '/send' 来开始， \n"
                     "回复某条消息 '/watermark <标签>' 向其发送已上传的图片， \n"
                     "私聊发送 '/cancel' 来取消， \n"
                     "私聊发送 '/modify <标签> <内容>' 来修改图片的描述， \n"
                     "发送 '/help' 来获取帮助";
        bot.getApi().sendMessage(message->chat->id, msg); });

    // Register the message handler
    bot.getEvents().onAnyMessage([&bot](Message::Ptr message)
                                 { handle_message(bot, message); });

    // Start the message delete thread
    jthread delete_messages_thread(pending_delete, ref(bot));
    delete_messages_thread.detach();

    // Start the bot
    printf("Bot username: %s\n", bot.getApi().getMe()->username.c_str());
    TgLongPoll longPoll(bot);
    while (running)
    {
        printf("Long poll started\n");
        longPoll.start();
    }
    return 0;
}
