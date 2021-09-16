# < insert cool poem here >

from rikerbot.CPluginLoader import PluginLoader
from rikerbot.PluginBase import PluginBase, on_events, pl_announce
from rikerbot import proto
from rikerbot.plugins.CEventCore import EventCore
from rikerbot.proto.Proto1_16_5 import ClientboundChat

class ChatMessage:
    def __init__(self, message):
        # Encode
        if isinstance(message, str):
            self.temp = "encode"

        # Decode
        elif isinstance(message, ClientboundChat):
            self.temp = "decode"

        self.temp = type(message)
        self.contents = message
        self.is_announcment = False
        self.sender = "The Man"


# Expands on the capabilities of the protocol implementation for easy to use chat messages.
class ChatPluginCore:
    def __init__(self, event_core: EventCore):
        self.chat_rcv_event = event_core.register_event('chat_received')

    # TODO: Move parse somewhere more appropriate
    # Create a ChatMessage object from a message packet
    def parse(message: ClientboundChat) -> ChatMessage:
        return ChatMessage(message)


    @on_events("ClientboundChat")
    def handle_inbound_chat(self, event_id, in_packet : ClientboundChat):
        msg = ChatPluginCore.parse(in_packet)
        self.event.emit(self.chat_rcv_event, msg)


@pl_announce("Chat")
class ChatPlugin(PluginBase):
    requires = ("Event", "IO")


    def __init__(self, ploader: PluginLoader, settings):
        super().__init__(ploader, settings)

        self.core = ChatPluginCore(self.event)
        ploader.provide("Chat", self.core)

        
