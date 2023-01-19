/*
    Source code of @vps_watermark_bot on Telegram

    Arguments:
    -t: (Required) The token of the bot
    -d: (Required) The channel to send the watermarked images to
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

    Commands:
    /start: Start the bot
    /send: Start sending
    /cancel: Cancel sending
    /watermark <datetime>: Reply to a message in the channel, and the bot will find the images with the same datetime and reply to the message with the watermarked images
    /help: Show help message

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

static vector<Message::Ptr> messages_to_delete; // Messages to delete after 5s
void delete_messages(Bot &bot) {
    while (running) {
        this_thread::sleep_for(chrono::seconds(1)); // Sleep for 1s
        for (const Message::Ptr &message : messages_to_delete) {
            try {
                if (message->date + 5 < chrono::system_clock::to_time_t(chrono::system_clock::now())) {
                    bot.getApi().deleteMessage(message->chat->id, message->messageId);
                    messages_to_delete.erase(remove(messages_to_delete.begin(), messages_to_delete.end(), message), messages_to_delete.end());
                }
            } catch (exception &e) {
                printf("Failed to delete message %d: %s\n", message->messageId, e.what());
            }
        }
    }
}

bool auth(int64 id)
{
    return any_of(config.uids.begin(), config.uids.end(), [&](int64 uid)
                  { return uid == id; });
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
            bot.getApi().sendMessage(message->chat->id, "Please send me the images first");
            return;
        }
        // Send the final message
        bot.getApi().sendMessage(message->chat->id, "Sending the images...");

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
        bot.getApi().sendMessage(message->chat->id, "Are you sure to send the images to the channel?", false, 0,
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
                            input_media->hasSpoiler = false;
                            media.push_back(input_media);
                        }
                        bot.getApi().sendMediaGroup(config.channel, media);
                        // Send the confirmation message
                        bot.getApi().sendMessage(s.uid, "Images sent to the channel");
                        bot.getApi().sendMessage(config.channel, "Tag: " + s.datetime);
                    } else {
                        // Send the confirmation message
                        bot.getApi().sendMessage(s.uid, "Canceled");
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
                        fstream f(config.save_path + "info.txt", ios::out);
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
    string filename = photo->fileId + ".jpg";
    try
    {
        auto file = bot.getApi().getFile(photo->fileId);
        auto f = fopen((s.path + filename).c_str(), "w");
        if (f == nullptr)
        {
            bot.getApi().sendMessage(message->chat->id, "Failed to open the file");
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
                bot.getApi().sendMessage(message->chat->id, "Failed to open the file");
                return;
            }
            fwrite(content.data(), 1, content.size(), f);
            fclose(f);
        }
    }
    catch (exception &e)
    {
        bot.getApi().sendMessage(message->chat->id, "Failed to download the photo");
        return;
    }

    // Add the watermark
    if (add_watermark(s.path + filename))
    {
        bot.getApi().sendMessage(message->chat->id, "Failed to add the watermark");
        return;
    }

    // Save the image
    s.images.push_back(filename);
    bot.getApi().sendMessage(message->chat->id, "Image added");
}

void handle_watermark(Bot &bot, Message::Ptr message)
{
    messages_to_delete.push_back(message);
    // This command must reply to a message
    if (message->replyToMessage == nullptr)
    {
        messages_to_delete.push_back(bot.getApi().sendMessage(message->chat->id, "Reply to a message to set the watermark"));
        return;
    }
    // Argument: a datetime string sent by the bot in "/send" command
    if (message->text.size() < 10)
    {
        messages_to_delete.push_back(bot.getApi().sendMessage(message->chat->id, "Invalid argument"));
        return;
    }
    string datetime = message->text.substr(10);
    if (datetime.size() != 19)
    {
        messages_to_delete.push_back(bot.getApi().sendMessage(message->chat->id, "Invalid argument"));
        return;
    }
    string path = config.save_path + datetime + "/";
    if (access(path.c_str(), F_OK) != 0)
    {
        messages_to_delete.push_back(bot.getApi().sendMessage(message->chat->id, "Invalid argument"));
        return;
    }
    // Read the info.txt to get photo file ids and the description
    fstream f(path + "info.txt");
    if (!f.is_open())
    {
        messages_to_delete.push_back(bot.getApi().sendMessage(message->chat->id, "Failed to open the file"));
        return;
    }
    try {
        // Datetime
        string datetime_f;
        getline(f, datetime);
        if (datetime_f.substr(5).compare(datetime) != 0) {
            throw exception();
        }

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
        getline(f, images_f);
        if (!images_f.starts_with("Images: ")) {
            throw exception();
        }
        vector<string> images;
        string image;
        istringstream iss(images_f.substr(8));
        while (getline(iss, image, ' ')) {
            images.push_back(image);
        }

        f.close();

        // Upload
        vector<InputMedia::Ptr> media;
        for (const string& img : images) {
            auto m = make_shared<InputMediaPhoto>();
            m->media = img;
            m->caption = description;
            m->hasSpoiler = false;
            media.push_back(m);
        }
        bot.getApi().sendMediaGroup(message->chat->id, media, false, message->replyToMessage->messageId);
        return;
    }
    catch (exception &e) {
        messages_to_delete.push_back(bot.getApi().sendMessage(message->chat->id, "Wrong file format"));
        return;
    }
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
            bot.getApi().sendMessage(message->chat->id, "You are not allowed to use this bot");
            return;
        }
        // Send the welcome message
        string msg = "Welcome to @" +  bot.getApi().getMe()->username + "!";
        bot.getApi().sendMessage(message->chat->id, msg);
        bot.getApi().sendMessage(message->chat->id, "Send me '/send' to start"); });

    // Register the /send command
    bot.getEvents().onCommand("send", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        bot.getApi().sendMessage(message->chat->id, 
            "Send me the images to add watermark"
            " and the final message to send to the channel");
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
                bot.getApi().sendMessage(message->chat->id, "Failed to create the directory");
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
        bot.getApi().sendMessage(message->chat->id, "Canceled"); });

    // Register the /watermark command
    bot.getEvents().onCommand("watermark", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        handle_watermark(bot, message); });

    // Register the /help command
    bot.getEvents().onCommand("help", [&bot](Message::Ptr message)
                              {
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }

        // Send the help message
        string msg = "Send me '/send' to start";
        bot.getApi().sendMessage(message->chat->id, msg); });

    // Register the message handler
    bot.getEvents().onAnyMessage([&bot](Message::Ptr message)
                                 { handle_message(bot, message); });

    // Start the message delete thread
    jthread delete_messages_thread(delete_messages, ref(bot));
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
