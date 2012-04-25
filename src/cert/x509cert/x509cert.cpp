/*
* X.509 Certificates
* (C) 1999-2010 Jack Lloyd
*
* Distributed under the terms of the Botan license
*/

#include <botan/x509cert.h>
#include <botan/x509_ext.h>
#include <botan/der_enc.h>
#include <botan/ber_dec.h>
#include <botan/internal/stl_util.h>
#include <botan/parsing.h>
#include <botan/bigint.h>
#include <botan/oids.h>
#include <botan/pem.h>
#include <botan/hex.h>
#include <algorithm>
#include <iterator>
#include <sstream>

namespace Botan {

namespace {

/*
* Lookup each OID in the vector
*/
std::vector<std::string> lookup_oids(const std::vector<std::string>& in)
   {
   std::vector<std::string> out;

   for(auto i = in.begin(); i != in.end(); ++i)
      out.push_back(OIDS::lookup(OID(*i)));
   return out;
   }

}

/*
* X509_Certificate Constructor
*/
X509_Certificate::X509_Certificate(DataSource& in) :
   X509_Object(in, "CERTIFICATE/X509 CERTIFICATE")
   {
   self_signed = false;
   do_decode();
   }

/*
* X509_Certificate Constructor
*/
X509_Certificate::X509_Certificate(const std::string& in) :
   X509_Object(in, "CERTIFICATE/X509 CERTIFICATE")
   {
   self_signed = false;
   do_decode();
   }

/*
* Decode the TBSCertificate data
*/
void X509_Certificate::force_decode()
   {
   size_t version;
   BigInt serial_bn;
   AlgorithmIdentifier sig_algo_inner;
   X509_DN dn_issuer, dn_subject;
   X509_Time start, end;

   BER_Decoder tbs_cert(tbs_bits);

   tbs_cert.decode_optional(version, ASN1_Tag(0),
                            ASN1_Tag(CONSTRUCTED | CONTEXT_SPECIFIC))
      .decode(serial_bn)
      .decode(sig_algo_inner)
      .decode(dn_issuer)
      .start_cons(SEQUENCE)
         .decode(start)
         .decode(end)
         .verify_end()
      .end_cons()
      .decode(dn_subject);

   if(version > 2)
      throw Decoding_Error("Unknown X.509 cert version " + std::to_string(version));
   if(sig_algo != sig_algo_inner)
      throw Decoding_Error("Algorithm identifier mismatch");

   self_signed = (dn_subject == dn_issuer);

   subject.add(dn_subject.contents());
   issuer.add(dn_issuer.contents());

   BER_Object public_key = tbs_cert.get_next_object();
   if(public_key.type_tag != SEQUENCE || public_key.class_tag != CONSTRUCTED)
      throw BER_Bad_Tag("X509_Certificate: Unexpected tag for public key",
                        public_key.type_tag, public_key.class_tag);

   MemoryVector<byte> v2_issuer_key_id, v2_subject_key_id;

   tbs_cert.decode_optional_string(v2_issuer_key_id, BIT_STRING, 1);
   tbs_cert.decode_optional_string(v2_subject_key_id, BIT_STRING, 2);

   BER_Object v3_exts_data = tbs_cert.get_next_object();
   if(v3_exts_data.type_tag == 3 &&
      v3_exts_data.class_tag == ASN1_Tag(CONSTRUCTED | CONTEXT_SPECIFIC))
      {
      Extensions extensions;

      BER_Decoder(v3_exts_data.value).decode(extensions).verify_end();

      extensions.contents_to(subject, issuer);
      }
   else if(v3_exts_data.type_tag != NO_OBJECT)
      throw BER_Bad_Tag("Unknown tag in X.509 cert",
                        v3_exts_data.type_tag, v3_exts_data.class_tag);

   if(tbs_cert.more_items())
      throw Decoding_Error("TBSCertificate has more items that expected");

   subject.add("X509.Certificate.version", version);
   subject.add("X509.Certificate.serial", BigInt::encode(serial_bn));
   subject.add("X509.Certificate.start", start.readable_string());
   subject.add("X509.Certificate.end", end.readable_string());

   issuer.add("X509.Certificate.v2.key_id", v2_issuer_key_id);
   subject.add("X509.Certificate.v2.key_id", v2_subject_key_id);

   subject.add("X509.Certificate.public_key",
               PEM_Code::encode(
                  ASN1::put_in_sequence(public_key.value),
                  "PUBLIC KEY"
                  )
      );

   if(is_CA_cert() &&
      !subject.has_value("X509v3.BasicConstraints.path_constraint"))
      {
      const size_t limit = (x509_version() < 3) ?
        Cert_Extension::NO_CERT_PATH_LIMIT : 0;

      subject.add("X509v3.BasicConstraints.path_constraint", limit);
      }
   }

/*
* Return the X.509 version in use
*/
u32bit X509_Certificate::x509_version() const
   {
   return (subject.get1_u32bit("X509.Certificate.version") + 1);
   }

/*
* Return the time this cert becomes valid
*/
std::string X509_Certificate::start_time() const
   {
   return subject.get1("X509.Certificate.start");
   }

/*
* Return the time this cert becomes invalid
*/
std::string X509_Certificate::end_time() const
   {
   return subject.get1("X509.Certificate.end");
   }

/*
* Return information about the subject
*/
std::vector<std::string>
X509_Certificate::subject_info(const std::string& what) const
   {
   return subject.get(X509_DN::deref_info_field(what));
   }

/*
* Return information about the issuer
*/
std::vector<std::string>
X509_Certificate::issuer_info(const std::string& what) const
   {
   return issuer.get(X509_DN::deref_info_field(what));
   }

/*
* Return the public key in this certificate
*/
Public_Key* X509_Certificate::subject_public_key() const
   {
   DataSource_Memory source(subject.get1("X509.Certificate.public_key"));
   return X509::load_key(source);
   }

/*
* Check if the certificate is for a CA
*/
bool X509_Certificate::is_CA_cert() const
   {
   if(!subject.get1_u32bit("X509v3.BasicConstraints.is_ca"))
      return false;
   if((constraints() & KEY_CERT_SIGN) || (constraints() == NO_CONSTRAINTS))
      return true;
   return false;
   }

/*
* Return the path length constraint
*/
u32bit X509_Certificate::path_limit() const
   {
   return subject.get1_u32bit("X509v3.BasicConstraints.path_constraint", 0);
   }

/*
* Return the key usage constraints
*/
Key_Constraints X509_Certificate::constraints() const
   {
   return Key_Constraints(subject.get1_u32bit("X509v3.KeyUsage",
                                              NO_CONSTRAINTS));
   }

/*
* Return the list of extended key usage OIDs
*/
std::vector<std::string> X509_Certificate::ex_constraints() const
   {
   return lookup_oids(subject.get("X509v3.ExtendedKeyUsage"));
   }

/*
* Return the list of certificate policies
*/
std::vector<std::string> X509_Certificate::policies() const
   {
   return lookup_oids(subject.get("X509v3.CertificatePolicies"));
   }

/*
* Return the authority key id
*/
MemoryVector<byte> X509_Certificate::authority_key_id() const
   {
   return issuer.get1_memvec("X509v3.AuthorityKeyIdentifier");
   }

/*
* Return the subject key id
*/
MemoryVector<byte> X509_Certificate::subject_key_id() const
   {
   return subject.get1_memvec("X509v3.SubjectKeyIdentifier");
   }

/*
* Return the certificate serial number
*/
MemoryVector<byte> X509_Certificate::serial_number() const
   {
   return subject.get1_memvec("X509.Certificate.serial");
   }

/*
* Return the distinguished name of the issuer
*/
X509_DN X509_Certificate::issuer_dn() const
   {
   return create_dn(issuer);
   }

/*
* Return the distinguished name of the subject
*/
X509_DN X509_Certificate::subject_dn() const
   {
   return create_dn(subject);
   }

namespace {

bool cert_subject_dns_match(const std::string& name,
                            const std::vector<std::string>& cert_names)
   {
   for(size_t i = 0; i != cert_names.size(); ++i)
      {
      const std::string cn = cert_names[i];

      if(cn == name)
         return true;

      /*
      * Possible wildcard match. We only support the most basic form of
      * cert wildcarding ala RFC 2595
      */
      if(cn.size() > 2 && cn[0] == '*' && cn[1] == '.' && name.size() > cn.size())
         {
         const std::string base = cn.substr(1, std::string::npos);

         if(name.compare(name.size() - base.size(), base.size(), base) == 0)
            return true;
         }
      }

   return false;
   }

}

bool X509_Certificate::matches_dns_name(const std::string& name) const
   {
   if(name == "")
      return false;

   if(cert_subject_dns_match(name, subject_info("DNS")))
      return true;

   if(cert_subject_dns_match(name, subject_info("Name")))
      return true;

   return false;
   }

/*
* Compare two certificates for equality
*/
bool X509_Certificate::operator==(const X509_Certificate& other) const
   {
   return (sig == other.sig &&
           sig_algo == other.sig_algo &&
           self_signed == other.self_signed &&
           issuer == other.issuer &&
           subject == other.subject);
   }

bool X509_Certificate::operator<(const X509_Certificate& other) const
   {
   /* If signature values are not equal, sort by lexicographic ordering of that */
   if(sig != other.sig)
      {
      if(sig < other.sig)
         return true;
      return false;
      }

   /*
   * same signatures, highly unlikely case, revert to compare
   * of entire contents
   */

   return to_string() < other.to_string();
   }

/*
* X.509 Certificate Comparison
*/
bool operator!=(const X509_Certificate& cert1, const X509_Certificate& cert2)
   {
   return !(cert1 == cert2);
   }

std::string X509_Certificate::to_string() const
   {
   const char* dn_fields[] = { "Name",
                               "Email",
                               "Organization",
                               "Organizational Unit",
                               "Locality",
                               "State",
                               "Country",
                               "IP",
                               "DNS",
                               "URI",
                               "PKIX.XMPPAddr",
                               0 };

   std::ostringstream out;

   for(size_t i = 0; dn_fields[i]; ++i)
      {
      const std::vector<std::string> vals = this->subject_info(dn_fields[i]);

      if(vals.empty())
         continue;

      out << "Subject " << dn_fields[i] << ":";
      for(size_t j = 0; j != vals.size(); ++j)
         out << " " << vals[j];
      out << "\n";
      }

   for(size_t i = 0; dn_fields[i]; ++i)
      {
      const std::vector<std::string> vals = this->issuer_info(dn_fields[i]);

      if(vals.empty())
         continue;

      out << "Issuer " << dn_fields[i] << ":";
      for(size_t j = 0; j != vals.size(); ++j)
         out << " " << vals[j];
      out << "\n";
      }

   out << "Version: " << this->x509_version() << "\n";

   out << "Not valid before: " << this->start_time() << "\n";
   out << "Not valid after: " << this->end_time() << "\n";

   out << "Constraints:\n";
   Key_Constraints constraints = this->constraints();
   if(constraints == NO_CONSTRAINTS)
      out << " None\n";
   else
      {
      if(constraints & DIGITAL_SIGNATURE)
         out << "   Digital Signature\n";
      if(constraints & NON_REPUDIATION)
         out << "   Non-Repuidation\n";
      if(constraints & KEY_ENCIPHERMENT)
         out << "   Key Encipherment\n";
      if(constraints & DATA_ENCIPHERMENT)
         out << "   Data Encipherment\n";
      if(constraints & KEY_AGREEMENT)
         out << "   Key Agreement\n";
      if(constraints & KEY_CERT_SIGN)
         out << "   Cert Sign\n";
      if(constraints & CRL_SIGN)
         out << "   CRL Sign\n";
      }

   std::vector<std::string> policies = this->policies();
   if(policies.size())
      {
      out << "Policies: " << "\n";
      for(size_t i = 0; i != policies.size(); i++)
         out << "   " << policies[i] << "\n";
      }

   std::vector<std::string> ex_constraints = this->ex_constraints();
   if(ex_constraints.size())
      {
      out << "Extended Constraints:\n";
      for(size_t i = 0; i != ex_constraints.size(); i++)
         out << "   " << ex_constraints[i] << "\n";
      }

   out << "Signature algorithm: " <<
      OIDS::lookup(this->signature_algorithm().oid) << "\n";

   out << "Serial number: " << hex_encode(this->serial_number()) << "\n";

   if(this->authority_key_id().size())
     out << "Authority keyid: " << hex_encode(this->authority_key_id()) << "\n";

   if(this->subject_key_id().size())
     out << "Subject keyid: " << hex_encode(this->subject_key_id()) << "\n";

   X509_PublicKey* pubkey = this->subject_public_key();
   out << "Public Key:\n" << X509::PEM_encode(*pubkey);
   delete pubkey;

   return out.str();
   }

/*
* Create and populate a X509_DN
*/
X509_DN create_dn(const Data_Store& info)
   {
   auto names = info.search_for(
      [](const std::string& key, const std::string&)
      {
         return (key.find("X520.") != std::string::npos);
      });

   X509_DN dn;

   for(auto i = names.begin(); i != names.end(); ++i)
      dn.add_attribute(i->first, i->second);

   return dn;
   }

/*
* Create and populate an AlternativeName
*/
AlternativeName create_alt_name(const Data_Store& info)
   {
   auto names = info.search_for(
      [](const std::string& key, const std::string&)
      {
         return (key == "RFC822" ||
                 key == "DNS" ||
                 key == "URI" ||
                 key == "IP");
      });

   AlternativeName alt_name;

   for(auto i = names.begin(); i != names.end(); ++i)
      alt_name.add_attribute(i->first, i->second);

   return alt_name;
   }

}
