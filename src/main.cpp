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
    - Images: <number of images>

    Commands:
    /start: Start the bot
    /send: Start sending
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
    bool sending;
    vector<string> images;
    int index;
    string datetime;
};

static map<int64, _status> status;

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
    // Crop the watermark to the same size as the image
    cv::Mat watermark = Watermark(cv::Rect(0, 0, image.cols, image.rows));
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
        auto t = message->text;
        if (s.images.empty())
        {
            bot.getApi().sendMessage(message->chat->id, "Please send me the images first");
            return;
        }
        // Send the final message
        bot.getApi().sendMessage(message->chat->id, "Sending the images...");
        // Send the images
        for (const string &image : s.images)
        {
            bot.getApi().sendPhoto(message->chat->id, InputFile::fromFile(image, "image/jpeg"));
        }
        // Send the final message
        bot.getApi().sendMessage(message->chat->id, t);
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
        bot.getEvents().onCallbackQuery([&](CallbackQuery::Ptr query)
                                        {
                    if (query->data == "yes") {
                        // Send the images
                        for (const string& image : s.images) {
                            bot.getApi().sendPhoto(config.channel, InputFile::fromFile(image, "image/jpeg"));
                        }
                        // Send the final message
                        bot.getApi().sendMessage(config.channel, t);
                        // Send the confirmation message
                        bot.getApi().sendMessage(message->chat->id, "Images sent to the channel");
                    } else {
                        // Send the confirmation message
                        bot.getApi().sendMessage(message->chat->id, "Canceled");
                    }
                    // Stop the confirmation
                    bot.getEvents().onCallbackQuery(nullptr);

                    // Clean up
                    if (!config.save_path.empty()) {
                        for (const string& image : s.images) {
                            remove(image.c_str());
                        }
                    } else {
                        // Write info.txt
                        string info = config.save_path + s.datetime + "/" + "info.txt";
                        auto f = fopen(info.c_str(), "w");
                        if (f == nullptr) {
                            bot.getApi().sendMessage(message->chat->id, "Failed to open the file");
                            return;
                        }
                        fprintf(f, "Time: %s\nUser: %ld\nMessage: %s\nImages: %ld", s.datetime.c_str(), message->from->id, t.c_str(), s.images.size());
                        fclose(f);
                    }
                    status.erase(message->from->id); });
        return;
    }

    // Get the photo
    const PhotoSize::Ptr photo = message->photo.back();
    // Download the photo
    string filename = photo->fileId + ".jpg";
    string path = "";
    if (!config.save_path.empty())
    {
        s.datetime = iso8601();
        path = config.save_path + s.datetime + "/";
        if (mkdir(path.c_str(), 0777) != 0)
        {
            bot.getApi().sendMessage(message->chat->id, "Failed to create the directory");
            return;
        }
    }
    try
    {
        auto file = bot.getApi().getFile(photo->fileId);
        auto f = fopen((path + filename).c_str(), "w");
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
            string original = path + "orig_" + filename;
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
    if (add_watermark(path + filename))
    {
        bot.getApi().sendMessage(message->chat->id, "Failed to add the watermark");
        return;
    }

    // Save the image
    s.images.push_back(filename);
    bot.getApi().sendMessage(message->chat->id, "Image added");
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
        status[message->from->id].sending = true; });

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
