/*
    Source code of @vps_watermark_bot on Telegram

    Arguments:
    -t: (Required) The token of the bot
    -d: (Required) The channel to send the watermarked images to 
    -u: (Required) Allowed UIDs (separated by commas ',')
    -w: The watermark image file to add to the images (default is watermark.png)
    -a: The alpha value of the watermark (default is 0.5)
    -s: Save the images to destination directory (default not to save them)

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

#include <tgbot/tgbot.h>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace TgBot;

static cv::Mat Watermark;
static vector<int64> uids;
static bool running = true;
static int64 current_uid = 0;
static vector<string> images;

bool auth(int64 id) {
    return any_of(uids.begin(), uids.end(), [&](int64 uid) {
        return uid == id;
    });
}

void signal_handler(const int signal) {
    printf("Received signal %d\n", signal);
    running = false;
    exit(0);
}
int add_watermark(const double& alpha, const string& img) {
    // Read the image
    cv::Mat image = cv::imread(img);
    if (image.empty()) {
        printf("Failed to read image %s\n", img.c_str());
        return 1;
    }
    // Resize the watermark
    cv::Mat watermark;
    cv::resize(Watermark, watermark, cv::Size(image.cols, image.rows));
    // Add the watermark
    cv::addWeighted(image, 1.0, watermark, alpha, 0.0, image);
    // Save the image
    cv::imwrite(img, image);
    return 0;
}

int main(const int argc, const char** argv) {
    // Parse arguments
    string token, channel, watermark, uids_s, save_path;
    double alpha = 0.5;
    for (int i = 1; i < argc; i++) {
        if (string(argv[i]) == "-t") {
            token = argv[++i];
        } else if (string(argv[i]) == "-d") {
            channel = argv[++i];
        } else if (string(argv[i]) == "-w") {
            watermark = argv[++i];
        } else if (string(argv[i]) == "-a") {
            alpha = atof(argv[++i]);
        } else if (string(argv[i]) == "-u") {
            uids_s = argv[++i];
        } else if (string(argv[i]) == "-s") {
            save_path = argv[++i];
        }
        else if (string(argv[i]) == "-h") {
            printf("Usage: %s -t <token> -d <channel> -u <uids> [-w <watermark>] [-a <alpha>]\n", argv[0]);
            return 0;
        } else {
            printf("Unknown argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (token.empty() || channel.empty() || uids_s.empty()) {
        printf("Usage: %s -t <token> -d <channel> -u <uids> [-w <watermark>] [-a <alpha>]\n", argv[0]);
        return 1;
    }

    // Check watermark file
    if (watermark.empty()) {
        watermark = "watermark.png";
    }
    if((Watermark = cv::imread(watermark)).empty()) {
        printf("Watermark file '%s' not found\n", watermark.c_str());
        return 1;
    }

    // Check save path
    bool save = !save_path.empty();
    if(save) {
        save_path.append("/");
        // Test if the directory is writable
        FILE const* f = fopen((save_path + "test").c_str(), "w");
        if (f == nullptr) {
            printf("Save path '%s' is not writable\n", save_path.c_str());
            return 1;
        }
    }

    // Parse UIDs
    size_t pos = 0;
    while (true) {
        size_t next = uids_s.find(',', pos);
        if (next == string::npos) {
            uids.push_back(atoi(uids_s.substr(pos).c_str()));
            break;
        }
        uids.push_back(atoll(uids_s.substr(pos, next - pos).c_str()));
        pos = next + 1;
    }

    // Print the arguments
    printf("Token: %s****\n", token.substr(0, 10).c_str()); // Hide part of token to keep secure
    printf("Destnation Channel: %s\n", channel.c_str());
    printf("Watermark file: %s\n", watermark.c_str());
    printf("Alpha: %f\n", alpha);
    printf("Allowed UIDs: ");
    for (auto uid : uids) {
        printf("%ld, ", uid);
    }
    printf("\n");
    if (save) {
        printf("Save path: %s\n", save_path.c_str());
    } else {
        printf("Save path: Not saving\n");
    }


    // Create the bot
    Bot bot(token);

    // Register the signal handler
    signal(SIGINT, signal_handler);

    // Register the /start command
    bot.getEvents().onCommand("start", [&](Message::Ptr message) {
        if (!auth(message->from->id)) {
            bot.getApi().sendMessage(message->chat->id, "You are not allowed to use this bot");
            return;
        }
        // Send the welcome message
        string msg = "Welcome to " +  bot.getApi().getMe()->username + "!";
        bot.getApi().sendMessage(message->chat->id, msg);
        bot.getApi().sendMessage(message->chat->id, "Send me '/send' to start", channel.c_str());
    });

    // Register the /send command
    bot.getEvents().onCommand("send", [&](Message::Ptr message) {
        /*
            Progress:
            1. Send the welcome message
            2. User sends the images
            3. User sends describing text as the final message
            4. Send the watermark-ed images and the final message
            5. Ask user to confirm using the inline keyboard
            6. If confirmed, send the images to the channel along with the final message
        */
        if (!auth(message->from->id)) {
            // Ignore the message
            return;
        }
        if (current_uid != 0) {
            bot.getApi().sendMessage(message->chat->id, "Another user is using the bot");
            return;
        }
        current_uid = message->from->id;

        bot.getApi().sendMessage(message->chat->id, 
            "Send me the images to add watermark"
            " and the final message to send to the channel");

        // Start the conversation
        bot.getEvents().onAnyMessage([&](Message::Ptr message_a) {
            // Check if the message is from the same user
            if (message_a->from->id != current_uid) {
                // Ignore the message
                return;
            }

            // Check if the message is a photo
            if (message_a->photo.empty()) {
                // Check if the message is a text
                if (message_a->text.empty()) {
                    // Ignore the messages
                    return;
                }
                if (images.empty()) {
                    bot.getApi().sendMessage(message_a->chat->id, "Please send me the images first");
                    return;
                }
                // Send the final message
                bot.getApi().sendMessage(message_a->chat->id, "Sending the images...");
                // Send the images
                for (const string& image : images) {
                    bot.getApi().sendPhoto(message_a->chat->id, InputFile::fromFile(image, "image/jpeg"));
                }
                // Send the final message
                bot.getApi().sendMessage(message_a->chat->id, message_a->text, channel.c_str());
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
                bot.getApi().sendMessage(message_a->chat->id, "Are you sure to send the images to the channel?", false, 0, 
                    keyboard, "Markdown");
                // Stop the conversation
                bot.getEvents().onAnyMessage(nullptr);
                // Start the confirmation
                bot.getEvents().onCallbackQuery([&](CallbackQuery::Ptr query) {
                    if (query->data == "yes") {
                        // Send the images
                        for (const string& image : images) {
                            bot.getApi().sendPhoto(channel, InputFile::fromFile(image, "image/jpeg"));
                        }
                        // Send the final message
                        bot.getApi().sendMessage(channel, message_a->text);
                        // Send the confirmation message
                        bot.getApi().sendMessage(message_a->chat->id, "Images sent to the channel");
                    } else {
                        // Send the confirmation message
                        bot.getApi().sendMessage(message_a->chat->id, "Canceled");
                    }
                    // Stop the confirmation
                    bot.getEvents().onCallbackQuery(nullptr);

                    // Clean up
                    if (!save) {
                        for (const string& image : images) {
                            remove(image.c_str());
                        }
                    }
                    images.clear();
                    current_uid = 0;
                });
                return;
            }

            // Get the photo
            const PhotoSize::Ptr photo = message_a->photo.back();
            // Download the photo
            string filename = photo->fileId + ".jpg";
            if(save) {
                filename = save_path + filename;
            }
            try {
                auto file = bot.getApi().getFile(photo->fileId);
                auto f = fopen(filename.c_str(), "w");
                if (f == nullptr) {
                    bot.getApi().sendMessage(message_a->chat->id, "Failed to open the file");
                    return;
                }
                auto content = bot.getApi().downloadFile(file->filePath);
                fwrite(content.data(), 1, content.size(), f);
                fclose(f);
            } catch (exception& e) {
                bot.getApi().sendMessage(message_a->chat->id, "Failed to download the photo");
                return;
            }

            // Add the watermark
            if (add_watermark(alpha, filename)) {
                bot.getApi().sendMessage(message_a->chat->id, "Failed to add the watermark");
                return;
            }

            // Save the image
            images.push_back(filename);
            bot.getApi().sendMessage(message_a->chat->id, "Image added");
        });
    });

    // Start the bot
    try {
        printf("Bot username: %s\n", bot.getApi().getMe()->username.c_str());
        TgLongPoll longPoll(bot);
        while (running) {
            printf("Long poll started\n");
            longPoll.start();
        }
    } catch (exception& e) {
        printf("Error: %s\n", e.what());
    }
    return 0;
}
