package icenet

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config.Parameters
import freechips.rocketchip.diplomacy.{LazyModule, LazyModuleImp, IdRange}
import freechips.rocketchip.util.DecoupledHelper
import freechips.rocketchip.tilelink._


class DDIOPerfCounter extends Bundle {
  val cycles = UInt(64.W)
  val cnt    = UInt(64.W)
  val hist   = Vec(16, UInt(64.W))
}

class StreamReadRequest extends Bundle {
  val address = UInt(48.W)
  val length = UInt(15.W)
  val partial = Bool()
}

class StreamReader(nXacts: Int, outFlits: Int, maxBytes: Int)
    (implicit p: Parameters) extends LazyModule {

  val core = LazyModule(new StreamReaderCore(nXacts, outFlits, maxBytes))
  val node = core.node

  lazy val module = new Impl
  class Impl extends LazyModuleImp(this) {
    val dataBits = core.module.dataBits

    val io = IO(new Bundle {
      val req = Flipped(Decoupled(new StreamReadRequest))
      val resp = Decoupled(Bool())
      val out = Decoupled(new StreamChannel(dataBits))
      val ddio = Output(new DDIOPerfCounter)
    })

    core.module.io.req <> io.req
    io.resp <> core.module.io.resp

    val buffer = Module(new ReservationBuffer(nXacts, outFlits, dataBits))
    buffer.io.alloc <> core.module.io.alloc
    buffer.io.in <> core.module.io.out

    val aligner = Module(new Aligner(dataBits))
    aligner.io.in <> buffer.io.out
    io.out <> aligner.io.out

    io.ddio <> core.module.io.ddio
  }
}

class StreamReaderCore(nXacts: Int, outFlits: Int, maxBytes: Int)
    (implicit p: Parameters) extends LazyModule {
  val node = TLClientNode(Seq(TLMasterPortParameters.v1(Seq(TLClientParameters(
    name = "stream-reader", sourceId = IdRange(0, nXacts))))))

  lazy val module = new Impl
  class Impl extends LazyModuleImp(this) {
    val (tl, edge) = node.out(0)
    val dataBits = tl.params.dataBits
    val beatBytes = dataBits / 8
    val byteAddrBits = log2Ceil(beatBytes)
    val addrBits = tl.params.addressBits
    val lenBits = 15

    require (edge.manager.minLatency > 0)

    val io = IO(new Bundle {
      val req = Flipped(Decoupled(new StreamReadRequest))
      val resp = Decoupled(Bool())
      val alloc = Decoupled(new ReservationBufferAlloc(nXacts, outFlits))
      val out = Decoupled(new ReservationBufferData(nXacts, dataBits))

      val ddio = Output(new DDIOPerfCounter)
    })

    val s_idle :: s_read :: s_resp :: Nil = Enum(3)
    val state = RegInit(s_idle)

    // Physical (word) address in memory
    val sendaddr = Reg(UInt(addrBits.W))
    // Number of words to send
    val sendlen  = Reg(UInt(lenBits.W))
    // 0 if last packet in sequence, 1 otherwise
    val sendpart = Reg(Bool())

    val xactBusy = RegInit(0.U(nXacts.W))
    val xactOnehot = PriorityEncoderOH(~xactBusy)
    val xactId = OHToUInt(xactOnehot)
    val xactLast = Reg(UInt(nXacts.W))
    val xactLeftKeep = Reg(Vec(nXacts, UInt(beatBytes.W)))
    val xactRightKeep = Reg(Vec(nXacts, UInt(beatBytes.W)))

    val reqSize = MuxCase(byteAddrBits.U,
      (log2Ceil(maxBytes) until byteAddrBits by -1).map(lgSize =>
          // Use the largest size (beatBytes <= size <= maxBytes)
          // s.t. sendaddr % size == 0 and sendlen > size
          (sendaddr(lgSize-1,0) === 0.U &&
            (sendlen >> lgSize.U) =/= 0.U) -> lgSize.U))
    val isLast = (xactLast >> tl.d.bits.source)(0) && edge.last(tl.d)
    val canSend = state === s_read && !xactBusy.andR

    val fullKeep = ~0.U(beatBytes.W)
    val loffset = Reg(UInt(byteAddrBits.W))
    val roffset = Reg(UInt(byteAddrBits.W))
    val lkeep = fullKeep << loffset
    val rkeep = fullKeep >> roffset
    val first = Reg(Bool())

    xactBusy := (xactBusy | Mux(tl.a.fire, xactOnehot, 0.U)) &
                    ~Mux(tl.d.fire && edge.last(tl.d),
                          UIntToOH(tl.d.bits.source), 0.U)

    val helper = DecoupledHelper(tl.a.ready, io.alloc.ready)

    io.req.ready := state === s_idle
    io.alloc.valid := helper.fire(io.alloc.ready, canSend)
    io.alloc.bits.id := xactId
    io.alloc.bits.count := (1.U << (reqSize - byteAddrBits.U))
    tl.a.valid := helper.fire(tl.a.ready, canSend)
    tl.a.bits := edge.Get(
      fromSource = xactId,
      toAddress = sendaddr,
      lgSize = reqSize)._2

    val outLeftKeep = xactLeftKeep(tl.d.bits.source)
    val outRightKeep = xactRightKeep(tl.d.bits.source)

    io.out.valid := tl.d.valid
    io.out.bits.id := tl.d.bits.source
    io.out.bits.data.data := tl.d.bits.data
    io.out.bits.data.keep := MuxCase(fullKeep, Seq(
      (edge.first(tl.d) && edge.last(tl.d)) -> (outLeftKeep & outRightKeep),
      edge.first(tl.d) -> outLeftKeep,
      edge.last(tl.d)  -> outRightKeep))
    io.out.bits.data.last := isLast
    tl.d.ready := io.out.ready
    io.resp.valid := state === s_resp
    io.resp.bits := true.B

    when (io.req.fire) {
      val req = io.req.bits
      val lastaddr = req.address + req.length
      val startword = req.address(addrBits-1, byteAddrBits)
      val endword = lastaddr(addrBits-1, byteAddrBits) +
                      Mux(lastaddr(byteAddrBits-1, 0) === 0.U, 0.U, 1.U)

      loffset := req.address(byteAddrBits-1, 0)
      roffset := Cat(endword, 0.U(byteAddrBits.W)) - lastaddr
      first := true.B

      sendaddr := Cat(startword, 0.U(byteAddrBits.W))
      sendlen  := Cat(endword - startword, 0.U(byteAddrBits.W))
      sendpart := req.partial
      state := s_read

      assert(req.length > 0.U, "request length must be >0")
    }

    when (tl.a.fire) {
      val reqBytes = 1.U << reqSize
      sendaddr := sendaddr + reqBytes
      sendlen  := sendlen - reqBytes
      when (sendlen === reqBytes) {
        xactLast := (xactLast & ~xactOnehot) | Mux(sendpart, 0.U, xactOnehot)
        xactRightKeep(xactId) := rkeep
        state := s_resp
      } .otherwise {
        xactLast := xactLast & ~xactOnehot
        xactRightKeep(xactId) := fullKeep
      }
      when (first) {
        first := false.B
        xactLeftKeep(xactId) := lkeep
      } .otherwise {
        xactLeftKeep(xactId) := fullKeep
      }
    }

    when (io.resp.fire) {
      state := s_idle
    }

    // NOTE : Currently maxBytes is set to 64 (cacheline size).
    // Hence, each tl request does not perform cross-cacheline access.
    val cycle = RegInit(0.U(64.W))
    cycle := cycle + 1.U


    /*
     * 0 : under 80
     * 1 <= n <= 20 : 80 + 5n - 5 <= cycles < 80 + 5n
     * 15 : over 185
     */
    val ddioRdHist = Seq.fill(16)(RegInit(0.U(64.W)))

    val ddioRdLat = RegInit(0.U(64.W))
    val xactStarts = Seq.fill(nXacts)(Module(new Queue(UInt(64.W), 1)))
    for (i <- 0 until nXacts) {
      xactStarts(i).io.enq.valid := false.B
      xactStarts(i).io.enq.bits  := 0.U
      xactStarts(i).io.deq.ready := false.B
    }

    for (i <- 0 until nXacts) {
      xactStarts(i).io.enq.valid := tl.a.fire && (i.U === tl.a.bits.source)
      xactStarts(i).io.enq.bits  := cycle
    }

    for (i <- 0 until nXacts) {
      xactStarts(i).io.deq.ready := tl.d.fire && (i.U === tl.d.bits.source)
      when (tl.d.fire && (i.U === tl.d.bits.source) && xactStarts(i).io.deq.valid) {
        val a2d_cycles = cycle - xactStarts(i).io.deq.bits

        when (a2d_cycles < 80.U) {
          ddioRdHist(0) := ddioRdHist(0) + 1.U
        } .elsewhen ((80.U <= a2d_cycles) && (a2d_cycles < 85.U)) {
          ddioRdHist(1) := ddioRdHist(1) + 1.U
        } .elsewhen ((85.U <= a2d_cycles) && (a2d_cycles < 90.U)) {
          ddioRdHist(2) := ddioRdHist(2) + 1.U
        } .elsewhen ((90.U <= a2d_cycles) && (a2d_cycles < 95.U)) {
          ddioRdHist(3) := ddioRdHist(3) + 1.U
        } .elsewhen ((95.U <= a2d_cycles) && (a2d_cycles < 100.U)) {
          ddioRdHist(4) := ddioRdHist(4) + 1.U
        } .elsewhen ((105.U <= a2d_cycles) && (a2d_cycles < 110.U)) {
          ddioRdHist(5) := ddioRdHist(5) + 1.U
        } .elsewhen ((110.U <= a2d_cycles) && (a2d_cycles < 115.U)) {
          ddioRdHist(6) := ddioRdHist(6) + 1.U
        } .elsewhen ((115.U <= a2d_cycles) && (a2d_cycles < 120.U)) {
          ddioRdHist(7) := ddioRdHist(7) + 1.U
        } .elsewhen ((120.U <= a2d_cycles) && (a2d_cycles < 125.U)) {
          ddioRdHist(8) := ddioRdHist(8) + 1.U
        } .elsewhen ((125.U <= a2d_cycles) && (a2d_cycles < 130.U)) {
          ddioRdHist(9) := ddioRdHist(9) + 1.U
        } .elsewhen ((130.U <= a2d_cycles) && (a2d_cycles < 135.U)) {
          ddioRdHist(10) := ddioRdHist(10) + 1.U
        } .elsewhen ((135.U <= a2d_cycles) && (a2d_cycles < 140.U)) {
          ddioRdHist(11) := ddioRdHist(11) + 1.U
        } .elsewhen ((140.U <= a2d_cycles) && (a2d_cycles < 145.U)) {
          ddioRdHist(12) := ddioRdHist(12) + 1.U
        } .elsewhen ((145.U <= a2d_cycles) && (a2d_cycles < 150.U)) {
          ddioRdHist(13) := ddioRdHist(13) + 1.U
        } .elsewhen ((150.U <= a2d_cycles) && (a2d_cycles < 155.U)) {
          ddioRdHist(14) := ddioRdHist(14) + 1.U
        } .otherwise {
          ddioRdHist(15) := ddioRdHist(15) + 1.U
        }
        ddioRdLat := ddioRdLat + a2d_cycles
        ddioRdCnt := ddioRdCnt + 1.U
      }
    }
    io.ddio.cycles := ddioRdLat
    io.ddio.cnt    := ddioRdCnt
    io.ddio.hist   := ddioRdHist
  }
}

class StreamWriteRequest extends Bundle {
  val address = UInt(48.W)
  val length = UInt(16.W)
}

class StreamWriter(nXacts: Int, maxBytes: Int)
    (implicit p: Parameters) extends LazyModule {
  val node = TLClientNode(Seq(TLMasterPortParameters.v1(Seq(TLClientParameters(
    name = "stream-writer", sourceId = IdRange(0, nXacts))))))

  lazy val module = new Impl
  class Impl extends LazyModuleImp(this) {
    val (tl, edge) = node.out(0)
    val dataBits = tl.params.dataBits
    val beatBytes = dataBits / 8
    val byteAddrBits = log2Ceil(beatBytes)
    val addrBits = tl.params.addressBits
    val lenBits = 16

    require (edge.manager.minLatency > 0)

    val io = IO(new Bundle {
      val req = Flipped(Decoupled(new StreamWriteRequest))
      val resp = Decoupled(UInt(lenBits.W))
      val in = Flipped(Decoupled(new StreamChannel(dataBits)))
      val ddio = Output(new DDIOPerfCounter)
    })

    val s_idle :: s_data :: s_resp :: Nil = Enum(3)
    val state = RegInit(s_idle)

    val length = Reg(UInt(lenBits.W))
    val baseAddr = Reg(UInt(addrBits.W))
    val offset = Reg(UInt(addrBits.W))
    val addrMerged = baseAddr + offset
    val bytesToSend = length - offset
    val baseByteOff = baseAddr(byteAddrBits-1, 0)
    val byteOff = addrMerged(byteAddrBits-1, 0)
    val extraBytes = Mux(baseByteOff === 0.U, 0.U, beatBytes.U - baseByteOff)

    val xactBusy = RegInit(0.U(nXacts.W))
    val xactOnehot = PriorityEncoderOH(~xactBusy)
    val xactId = OHToUInt(xactOnehot)

    val maxBeats = maxBytes / beatBytes
    val beatIdBits = log2Ceil(maxBeats)

    val beatsLeft = Reg(UInt(beatIdBits.W))
    val headAddr = Reg(UInt(addrBits.W))
    val headXact = Reg(UInt(log2Ceil(nXacts).W))
    val headSize = Reg(UInt(log2Ceil(maxBytes + 1).W))

    val newBlock = beatsLeft === 0.U
    val canSend = !xactBusy.andR || !newBlock

    val reqSize = MuxCase(0.U,
      (log2Ceil(maxBytes) until 0 by -1).map(lgSize =>
          (addrMerged(lgSize-1,0) === 0.U &&
            (bytesToSend >> lgSize.U) =/= 0.U) -> lgSize.U))

    xactBusy := (xactBusy | Mux(tl.a.fire && newBlock, xactOnehot, 0.U)) &
                    ~Mux(tl.d.fire, UIntToOH(tl.d.bits.source), 0.U)

    val overhang = RegInit(0.U(dataBits.W))
    val sendTrail = bytesToSend <= extraBytes
    val fulldata = (overhang | (io.in.bits.data << Cat(baseByteOff, 0.U(3.W))))

    val fromSource = Mux(newBlock, xactId, headXact)
    val toAddress = Mux(newBlock, addrMerged, headAddr)
    val lgSize = Mux(newBlock, reqSize, headSize)
    val wdata = fulldata(dataBits-1, 0)
    val wmask = Cat((0 until beatBytes).map(
      i => (i.U >= byteOff) && (i.U < bytesToSend)).reverse)
    val wpartial = !wmask.andR

    val putPartial = edge.Put(
      fromSource = xactId,
      toAddress = addrMerged & ~(beatBytes-1).U(addrBits.W),
      lgSize = log2Ceil(beatBytes).U,
      data = Mux(sendTrail, overhang, wdata),
      mask = wmask)._2

    val putFull = edge.Put(
      fromSource = fromSource,
      toAddress = toAddress,
      lgSize = lgSize,
      data = wdata)._2

    io.req.ready := state === s_idle
    tl.a.valid := (state === s_data) && (io.in.valid || sendTrail) && canSend
    tl.a.bits := Mux(wpartial, putPartial, putFull)
    tl.d.ready := xactBusy.orR
    io.in.ready := state === s_data && canSend && !sendTrail && tl.a.ready
    io.resp.valid := state === s_resp && !xactBusy.orR
    io.resp.bits := length

    when (io.req.fire) {
      offset := 0.U
      baseAddr := io.req.bits.address
      length := io.req.bits.length
      beatsLeft := 0.U
      state := s_data
    }

    when (tl.a.fire) {
      when (!newBlock) {
        beatsLeft := beatsLeft - 1.U
      } .elsewhen (reqSize > byteAddrBits.U) {
        val nBeats = 1.U << (reqSize - byteAddrBits.U)
        beatsLeft := nBeats - 1.U
        headAddr := addrMerged
        headXact := xactId
        headSize := reqSize
      }

      val bytesSent = PopCount(wmask)
      offset := offset + bytesSent
      overhang := fulldata >> dataBits.U

      when (bytesSent === bytesToSend) { state := s_resp }
    }

    when (io.resp.fire) { state := s_idle }

    // NOTE : Currently maxBytes is set to 64 (cacheline size).
    // Hence, each tl request does not perform cross-cacheline access.
    val cycle = RegInit(0.U(64.W))
    cycle := cycle + 1.U

    /*
     * 0 : under 80
     * 1 <= n <= 20 : 80 + 5n - 5 <= cycles < 80 + 5n
     * 15 : over 185
     */
    val ddioWrHist = Seq.fill(16)(RegInit(0.U(64.W)))

    val ddioWrLat = RegInit(0.U(64.W))
    val ddioWrCnt = RegInit(0.U(64.W))
    val xactStarts = Seq.fill(nXacts)(Module(new Queue(UInt(64.W), 1)))
    for (i <- 0 until nXacts) {
      xactStarts(i).io.enq.valid := false.B
      xactStarts(i).io.enq.bits  := 0.U
      xactStarts(i).io.deq.ready := false.B
    }

    for (i <- 0 until nXacts) {
      xactStarts(i).io.enq.valid := tl.a.fire && (i.U === tl.a.bits.source)
      xactStarts(i).io.enq.bits  := cycle
    }

    for (i <- 0 until nXacts) {
      xactStarts(i).io.deq.ready := tl.d.fire && (i.U === tl.d.bits.source)
      when (tl.d.fire && (i.U === tl.d.bits.source) && xactStarts(i).io.deq.valid) {
        val a2d_cycles = (cycle - xactStarts(i).io.deq.bits)

        when (a2d_cycles < 80.U) {
          ddioWrHist(0) := ddioWrHist(0) + 1.U
        } .elsewhen ((80.U <= a2d_cycles) && (a2d_cycles < 85.U)) {
          ddioWrHist(1) := ddioWrHist(1) + 1.U
        } .elsewhen ((85.U <= a2d_cycles) && (a2d_cycles < 90.U)) {
          ddioWrHist(2) := ddioWrHist(2) + 1.U
        } .elsewhen ((90.U <= a2d_cycles) && (a2d_cycles < 95.U)) {
          ddioWrHist(3) := ddioWrHist(3) + 1.U
        } .elsewhen ((95.U <= a2d_cycles) && (a2d_cycles < 100.U)) {
          ddioWrHist(4) := ddioWrHist(4) + 1.U
        } .elsewhen ((105.U <= a2d_cycles) && (a2d_cycles < 110.U)) {
          ddioWrHist(5) := ddioWrHist(5) + 1.U
        } .elsewhen ((110.U <= a2d_cycles) && (a2d_cycles < 115.U)) {
          ddioWrHist(6) := ddioWrHist(6) + 1.U
        } .elsewhen ((115.U <= a2d_cycles) && (a2d_cycles < 120.U)) {
          ddioWrHist(7) := ddioWrHist(7) + 1.U
        } .elsewhen ((120.U <= a2d_cycles) && (a2d_cycles < 125.U)) {
          ddioWrHist(8) := ddioWrHist(8) + 1.U
        } .elsewhen ((125.U <= a2d_cycles) && (a2d_cycles < 130.U)) {
          ddioWrHist(9) := ddioWrHist(9) + 1.U
        } .elsewhen ((130.U <= a2d_cycles) && (a2d_cycles < 135.U)) {
          ddioWrHist(10) := ddioWrHist(10) + 1.U
        } .elsewhen ((135.U <= a2d_cycles) && (a2d_cycles < 140.U)) {
          ddioWrHist(11) := ddioWrHist(11) + 1.U
        } .elsewhen ((140.U <= a2d_cycles) && (a2d_cycles < 145.U)) {
          ddioWrHist(12) := ddioWrHist(12) + 1.U
        } .elsewhen ((145.U <= a2d_cycles) && (a2d_cycles < 150.U)) {
          ddioWrHist(13) := ddioWrHist(13) + 1.U
        } .elsewhen ((150.U <= a2d_cycles) && (a2d_cycles < 155.U)) {
          ddioWrHist(14) := ddioWrHist(14) + 1.U
        } .otherwise {
          ddioWrHist(15) := ddioWrHist(15) + 1.U
        }
        ddioWrLat := ddioWrLat + (cycle - xactStarts(i).io.deq.bits)
        ddioWrCnt := ddioWrCnt + 1.U
      }
    }
    io.ddio.cycles := ddioWrLat
    io.ddio.cnt    := ddioWrCnt
    io.ddio.hist   := ddioWrHist
  }
}
