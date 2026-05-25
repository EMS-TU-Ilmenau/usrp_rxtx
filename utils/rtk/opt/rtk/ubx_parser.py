#!/usr/bin/python3
# -*- coding: utf-8 -*-
# SPDX-License-Identifier: MIT
# (c) 2024 TU Ilmenau, FG EMS, Carsten Andrich <carsten.andrich@tu-ilmenau.de>

import asyncio
import enum
import io
import struct
from typing import Self


class IncompleteFrame(Exception):
    pass


class InvalidFrame(Exception):
    pass


class InvalidMessage(Exception):
    pass


class UnknownMessage(Exception):
    pass


class UbxClass(enum.Enum):
    ACK = 0x05
    CFG = 0x06
    INF = 0x04
    LOG = 0x21
    MGA = 0x13
    MON = 0x0a
    NAV = 0x01
    NAV2 = 0x29
    RXM = 0x02
    SEC = 0x27
    TIM = 0x0d
    UPD = 0x09


class UbxMsgType(enum.Enum):
    UNKNOWN = None
    ACK_ACK = (UbxClass.ACK.value, 0x01)
    ACK_NAK = (UbxClass.ACK.value, 0x00)
    CFG_VALDEL = (UbxClass.CFG.value, 0x8c)
    CFG_VALGET = (UbxClass.CFG.value, 0x8b)
    CFG_VALSET = (UbxClass.CFG.value, 0x8a)
    MON_VER = (UbxClass.MON.value, 0x04)
    NAV_PVT = (UbxClass.NAV.value, 0x07)
    NAV_RELPOSNED = (UbxClass.NAV.value, 0x3c)


# dict of messages by message type (populated at end of file)
MESSAGES: dict[UbxMsgType] = {}


class UbxMessage:
    CLASS = None
    ID = None

    def __init__(self, msg: bytearray | bytes):
        self.__dict__.update(zip(self.NAMES, self.MESSAGE.unpack(msg)))


class UbxFrame:
    PREAMB0 = 0xb5
    PREAMBLE = b"\xb5\x62"
    STRUCT = struct.Struct("<2sBBH")
    MAX_LEN = 2048
    TYPE = UbxMsgType.UNKNOWN

    def __init__(self, ubx_cls: int, ubx_id: int, payload: bytes):
        self.cls = ubx_cls
        self.id = ubx_id
        self.payload = payload

    @classmethod
    def from_bytearray(cls, buf: bytearray) -> Self:
        # buffer must hold entire header
        if len(buf) < cls.STRUCT.size:
            raise IncompleteFrame()

        # unpack frame and check preamble
        preamble, ubx_cls, ubx_id, length = cls.STRUCT.unpack_from(buf)
        if preamble != cls.PREAMBLE:
            raise InvalidFrame()
        if length > cls.MAX_LEN:
            raise InvalidFrame()

        # buffer must hold entire frame (header + payload + checksum)
        if len(buf) < length + 8:
            raise IncompleteFrame()

        # compute checksum
        ck_a, ck_b = 0, 0
        for n in range(2, cls.STRUCT.size + length):
            ck_a = (ck_a + buf[n]) & 0xFF
            ck_b = (ck_b + ck_a)   & 0xFF

        # compare checksums
        if ck_a != buf[length + 6] \
        or ck_b != buf[length + 7]:
            raise InvalidFrame()

        # copy message payload
        payload = buf[cls.STRUCT.size:cls.STRUCT.size + length]

        return cls(ubx_cls, ubx_id, payload), length + 8


    def parse(self) -> Self | UbxMessage:
        try:
            msgtype = UbxMsgType((self.cls, self.id))
        except ValueError:
            return self

        return MESSAGES[msgtype](self.payload)

    def serialize(self) -> bytearray:
        # preallocate buffer
        buf = bytearray(self.STRUCT.size + len(self.payload) + 2)

        # write header and payload
        self.STRUCT.pack_into(buf, 0, self.PREAMBLE, self.cls, self.id, len(self.payload))
        buf[self.STRUCT.size:self.STRUCT.size + len(self.payload)] = self.payload

        # compute checksum
        ck_a, ck_b = 0, 0
        for n in range(2, len(buf) - 2):
            ck_a = (ck_a + buf[n]) & 0xFF
            ck_b = (ck_b + ck_a)   & 0xFF

        # write checksum
        buf[-2] = ck_a
        buf[-1] = ck_b

        return buf


class UbxAckAck(UbxMessage):
    CLASS = UbxClass.ACK
    TYPE = UbxMsgType.ACK_ACK
    MESSAGE = struct.Struct("<BB")
    NAMES = ("_cls", "_id")


class UbxAckNak(UbxMessage):
    CLASS = UbxClass.ACK
    TYPE = UbxMsgType.ACK_NAK
    MESSAGE = struct.Struct("<BB")
    NAMES = ("_cls", "_id")


class UbxCfgValdel:
    CLASS = UbxClass.CFG
    TYPE = UbxMsgType.CFG_VALDEL

    class LAYER(enum.Enum):
        #RAM = 0b001
        BBR = 0b010
        FLASH = 0b100


class UbxCfgValget:
    CLASS = UbxClass.CFG
    TYPE = UbxMsgType.CFG_VALGET

    class LAYER(enum.Enum):
        RAM = 0
        BBR = 1
        FLASH = 2
        DEFAULT = 7


class UbxCfgValgetPoll(UbxCfgValget):
    MESSAGE = struct.Struct("<BBH")
    VERSION = 0

    def __init__(self, layer: UbxCfgValget.LAYER, position: int, keys: list[int]):
        if len(keys) > 64:
            raise ValueError()

        self._layer = layer
        self._position = position
        self._keys = keys

    def serialize(self) -> bytearray:
        buf = bytearray(self.MESSAGE.size + 4 * len(self._keys))
        self.MESSAGE.pack_into(buf, 0, self.VERSION, self._layer.value, self._position)
        for idx, key in enumerate(self._keys):
            struct.pack_into("<L", buf, self.MESSAGE.size + 4*idx, key)
        return UbxFrame(*self.TYPE.value, buf).serialize()


class UbxCfgValgetPolled(UbxCfgValget):
    MESSAGE = struct.Struct("<BBH")
    NAMES = ("_version", "_layer", "_position")
    VERSION = 0

    def __init__(self, msg: bytearray | bytes):
        # parse header
        self.__dict__.update(zip(self.NAMES, self.MESSAGE.unpack_from(msg, 0)))

        # parse config data
        # FIXME: malformed config data is not properly handled
        self._data = {}
        offset = self.MESSAGE.size
        while offset < len(msg):
            # read key
            key = struct.unpack_from("<L", msg, offset)[0]
            offset += 4

            # read value
            match key >> 28:
                case 1:
                    # FIXME: silently accepts invalid values >1 as True
                    val = bool(struct.unpack_from("<B", msg, offset)[0])
                    offset += 1
                case 2:
                    val = struct.unpack_from("<B", msg, offset)[0]
                    offset += 1
                case 3:
                    val = struct.unpack_from("<H", msg, offset)[0]
                    offset += 2
                case 4:
                    val = struct.unpack_from("<L", msg, offset)[0]
                    offset += 4
                case 5:
                    val = struct.unpack_from("<Q", msg, offset)[0]
                    offset += 8
                case _:
                    raise InvalidMessage(f"unknown key type 0x{key:02x}")

            self._data[key] = val


class UbxCfgValset:
    CLASS = UbxClass.CFG
    TYPE = UbxMsgType.CFG_VALSET
    MESSAGE = struct.Struct("<BBBx")
    NAMES = ("_version", "_layer", "_transaction")
    VERSION = 1

    class LAYER(enum.Enum):
        RAM = 0b001
        BBR = 0b010
        FLASH = 0b100

    class TRANSACTION(enum.Enum):
        NONE = 0
        START = 1
        ONGOING = 2
        APPLY = 3

    def __init__(self, layers: set, transaction, data: list[int]):
        if len(data) > 64:
            raise ValueError()

        self._layers = layers
        self._transaction = transaction
        self._data = data

    def serialize(self) -> bytearray:
        # compute message length
        data_length = 0
        for key, val in self._data.items():
            match key >> 28:
                case 1:
                    data_length += 4+1
                case 2:
                    data_length += 4+1
                case 3:
                    data_length += 4+2
                case 4:
                    data_length += 4+4
                case 5:
                    data_length += 4+8
                case _:
                    raise InvalidMessage(f"unknown key type 0x{key:02x}")

        # allocate buffer and pack header
        buf = bytearray(self.MESSAGE.size + data_length)
        layer = sum(lay.value if lay in self._layers else 0 for lay in self.LAYER)
        self.MESSAGE.pack_into(buf, 0, self.VERSION, layer, self._transaction.value)

        # sequentially pack keys and values
        offset = self.MESSAGE.size
        for key, val in self._data.items():
            match key >> 28:
                case 1:
                    # FIXME: silently accepts invalid values >1 as True
                    struct.pack_into("<LB", buf, offset, key, val)
                    offset += 4+1
                case 2:
                    struct.pack_into("<LB", buf, offset, key, val)
                    offset += 4+1
                case 3:
                    struct.pack_into("<LH", buf, offset, key, val)
                    offset += 4+2
                case 4:
                    struct.pack_into("<LL", buf, offset, key, val)
                    offset += 4+4
                case 5:
                    struct.pack_into("<LQ", buf, offset, key, val)
                    offset += 4+8
                case _:
                    raise InvalidMessage(f"unknown key type 0x{key:02x}")

        return UbxFrame(*self.TYPE.value, buf).serialize()


class UbxNavPvt(UbxMessage):
    CLASS = UbxClass.NAV
    TYPE = UbxMsgType.NAV_PVT
    MESSAGE = struct.Struct("<LHBBBBBBLlBBBBllllLLlllllLLHH4xlhH")
    NAMES = ("_iTOW", "_year", "_month", "_day", "_hour", "_min", "_sec", "_valid", "_tAcc", "_nano", "_fixType", "_flags", "_flags2", "_numSV", "_lon", "_lat", "_height", "_hMSL", "_hAcc", "_vAcc", "_velN", "_velE", "_velD", "_gSpeed", "_headMot", "_sAcc", "_headAcc", "_pDOP", "_flags3", "_headVeh", "_magDec", "_magAcc")

    class CARRSOLN(enum.Enum):
        """no carrier phase range solution"""
        NONE = 0
        """carrier phase range solution with ﬂoating ambiguities"""
        FLOAT = 1
        """carrier phase range solution with ﬁxed ambiguities"""
        FIXED = 2
        """invalid value"""
        INVAL = None

    class FIXTYPE(enum.Enum):
        """no fix"""
        NONE = 0
        """dead reckoning only"""
        DR_ONLY = 1
        """2D-fix"""
        FIX2D = 2
        """3D-fix"""
        FIX3D = 3
        """GNSS + dead reckoning combined"""
        GNSS_DR = 4
        """time only fix"""
        TIME = 5
        """invalid value"""
        INVAL = None

    class CORRECTIONAGE(enum.Enum):
        """Not available"""
        UNAVAIL = 0
        """Age between 0 and 1 second"""
        AGE1 = 1
        """Age between 1 (inclusive) and 2 seconds"""
        AGE2 = 2
        """Age between 2 (inclusive) and 5 seconds"""
        AGE5 = 3
        """Age between 5 (inclusive) and 10 seconds"""
        AGE10 = 4
        """Age between 10 (inclusive) and 15 seconds"""
        AGE15 = 5
        """Age between 15 (inclusive) and 20 seconds"""
        AGE20 = 6
        """Age between 20 (inclusive) and 30 seconds"""
        AGE30 = 7
        """Age between 30 (inclusive) and 45 seconds"""
        AGE45 = 8
        """Age between 45 (inclusive) and 60 seconds"""
        AGE60 = 9
        """Age between 60 (inclusive) and 90 seconds"""
        AGE90 = 10
        """Age between 90 (inclusive) and 120 seconds"""
        AGE120 = 11
        """Age greater or equal than 120 seconds"""
        OLD = 12
        """invalid value"""
        INVAL = None

    @property
    def acc_3d(self) -> float:
        return ((self._hAcc * 1e-3)**2 + (self._vAcc * 1e-3)**2)**0.5

    @property
    def carrSoln(self) -> CARRSOLN:
        try:
            return self.CARRSOLN(self._flags >> 6)
        except ValueError:
            return self.CARRSOLN.INVAL

    @property
    def lastCorrectionAge(self) -> CORRECTIONAGE:
        try:
            return self.CORRECTIONAGE((self._flags3 & 0x001e) >> 1)
        except ValueError:
            return self.CORRECTIONAGE.INVAL

    @property
    def fixType(self) -> bool:
        try:
            return self.FIXTYPE(self._fixType)
        except ValueError:
            return self.FIXTYPE.INVAL

    @property
    def gnssFixOK(self) -> bool:
        """valid fix (i.e within DOP & accuracy masks)"""
        return bool(self._flags & 0x01)

    @property
    def hMSL(self) -> float:
        """Height above mean sea level (m)"""
        return self._hMSL * 1e-3

    @property
    def lat(self) -> float:
        """Latitute (deg)"""
        return self._lat * 1e-7

    @property
    def lon(self) -> float:
        """Longitude (deg)"""
        return self._lon * 1e-7


class UbxNavRelposned(UbxMessage):
    CLASS = UbxClass.NAV
    TYPE = UbxMsgType.NAV_RELPOSNED
    MESSAGE = struct.Struct("<BxHLlllll4xbbbbLLLLL4xL")
    NAMES = ("_version", "_refStationId", "_iTOW", "_relPosN", "_relPosE", "_relPosD", "_relPosLength", "_relPosHeading", "_relPosHPN", "_relPosHPE", "_relPosHPD", "_relPosHPLength", "_accN", "_accE", "_accD", "_accLength", "_accHeading", "_flags")
    assert MESSAGE.size == 64

    @property
    def relPosN(self) -> float:
        return self._relPosN * 1e-2 + self._relPosHPN * 1e-4

    @property
    def relPosE(self) -> float:
        return self._relPosE * 1e-2 + self._relPosHPE * 1e-4

    @property
    def relPosD(self) -> float:
        return self._relPosD * 1e-2 + self._relPosHPD * 1e-4


# populate list of message with subclasses of Message with an integer .TYPE
import inspect
import sys
for name, obj in inspect.getmembers(sys.modules[__name__]):
    if hasattr(obj, "__bases__") and issubclass(obj, UbxMessage) \
    and isinstance(getattr(obj, "TYPE", None), UbxMsgType):
        MESSAGES[obj.TYPE] = obj

# FIXME: hack
MESSAGES[UbxMsgType.CFG_VALGET] = UbxCfgValgetPolled


class AsyncUbxParser:
    def __init__(self, reader: asyncio.StreamReader):
        self._buf = bytearray()
        self._reader = reader

    def __aiter__(self) -> Self:
        return self

    async def __anext__(self) -> UbxFrame | UbxMessage:
        while True:
            # read into temporary buffer and append to buf
            if len(self._buf) < 8:
                tmp = await self._reader.read(4096)
                self._buf += tmp
                if len(tmp) == 0:
                    raise EOFError

            try:
                # parse UBX frame
                if self._buf[0] == UbxFrame.PREAMB0:
                    frm, length = UbxFrame.from_bytearray(self._buf)
                    del self._buf[:length]
                    msg = frm.parse()
                # parse NMEA frame
                elif self._buf[0] == ord("$"):
                    raise NotImplementedError
                # parse RTCM frame
                elif self._buf[0] == 0xd3:
                    raise NotImplementedError
                else:
                    raise InvalidFrame
            except IncompleteFrame:
                tmp = await self._reader.read(4096)
                self._buf += tmp
                if len(tmp) == 0:
                    raise EOFError from None
                continue
            except (InvalidFrame, NotImplementedError):
                # discard unknown byte and proceed with next
                del self._buf[0]
                continue
            except UnknownMessage:
                continue

            return msg


class UbxParser:
    def __init__(self, file: io.FileIO):
        self._buf = bytearray()
        self._file = file

    def __iter__(self) -> Self:
        return self

    def __next__(self) -> UbxFrame | UbxMessage:
        while True:
            # read into temporary buffer and append to buf
            if len(self._buf) < 8:
                tmp = self._file.read(4096)
                self._buf += tmp
                if len(tmp) == 0:
                    raise EOFError

            try:
                # parse UBX frame
                if self._buf[0] == UbxFrame.PREAMB0:
                    frm, length = UbxFrame.from_bytearray(self._buf)
                    del self._buf[:length]
                    msg = frm.parse()
                # parse NMEA frame
                elif self._buf[0] == ord("$"):
                    raise NotImplementedError
                # parse RTCM frame
                elif self._buf[0] == 0xd3:
                    raise NotImplementedError
                else:
                    raise InvalidFrame
            except IncompleteFrame:
                tmp = self._file.read(4096)
                self._buf += tmp
                if len(tmp) == 0:
                    raise EOFError from None
                continue
            except (InvalidFrame, NotImplementedError):
                # discard unknown byte and proceed with next
                del self._buf[0]
                continue
            except UnknownMessage:
                continue

            return msg

